#include "ble_transport.h"
#include "hid_core.h"
#include "ble_device_info.h"
#include "ble_hid.h"
#include "ble_uuid.h"
#include "ble_ota.h"
#include "ble_control.h"

#include <assert.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"

#include "os/os_mbuf.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// Provided by the NimBLE store component; installs the NVS-backed callbacks
// required to save and retrieve bonding material.
void ble_store_config_init(void);

//--------------------------Variables-----------------------------
static const char *TAG = "BLE";

typedef struct {
    uint16_t conn_handle;
    bool encrypted;
} ble_connection_t;

static uint8_t own_addr_type;
static ble_connection_t connection;

// ---------------------Function Prototypes-----------------------

static void ble_init_nvs(void);
static void ble_init_host(void);
static void ble_init_security(void);
static void ble_init_services(void);
static void ble_gatt_init(void);

static void ble_host_task(void *param);
static void ble_on_reset(int reason);
static void ble_on_sync(void);
static int ble_gap_event(struct ble_gap_event *event, void *arg);

static void ble_start_advertising(void);  
static ble_connection_t *ble_find_connection(uint16_t conn_handle);
static bool ble_add_connection(uint16_t conn_handle);
static void ble_remove_connection(uint16_t conn_handle);

//---------------------------------------------------------------
//---------------------FUNCTION DEFINITIONS----------------------
//---------------------------------------------------------------

//------------INITIALIZATION-------------
static void ble_init_nvs(void){
    esp_err_t ret;

    ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "NVS initialized");
}

static void ble_init_host(void){
    int rc;

    rc = nimble_port_init();
    ESP_ERROR_CHECK(rc);

    ESP_LOGI(TAG, "NimBLE initialized");
}

static void ble_init_security(void){
    /*
     * Production BLE baseline:
     * - bonded, encrypted links only;
     * - bonded, encrypted access compatible with current Windows and phone
     *   HID pairing flows.
     *
     * The current hardware has neither a display nor a dedicated pairing
     * confirmation input, so its pairing association remains Just Works.
     * That gives confidentiality but not MITM authentication. Enabling the
     * Secure Connections request on the current HID pairing path causes hosts
     * to terminate pairing, so leave it disabled until a dedicated SC pairing
     * test and a verified pairing UI (QR/NFC OOB, display passkey, or physical
     * confirmation) are available. Do not enable sm_mitm / SC-only before it.
     */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_store_config_init();

    ESP_LOGI(TAG, "BLE bonding and encrypted access configured");
}

static void ble_init_services(void){
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ESP_LOGI(TAG, "GAP/GATT services initialized");
}

static void ble_gatt_init(void){
    int rc;
    /*
     * Keep the GATT database identical in normal and OTA modes.  Windows
     * caches attribute handles for bonded devices; changing the service list
     * after reboot makes it issue writes to stale handles.
     */
    rc = ble_gatts_count_cfg(hid_gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_count_cfg(control_gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_count_cfg(ota_gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_count_cfg(device_info_gatt_svcs);
    assert(rc == 0);

    rc = ble_gatts_add_svcs(hid_gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(control_gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(ota_gatt_svcs);
    assert(rc == 0);
    // Append new services so existing cached HID/control/OTA handles remain stable.
    rc = ble_gatts_add_svcs(device_info_gatt_svcs);
    assert(rc == 0);

    ESP_LOGI(TAG, "HID, control, OTA, and device-info GATT services registered");
}

//------------GAP CALL BACKS--------------
static void ble_host_task(void *param){
    ESP_LOGI(TAG, "BLE Host Task Started");
    // Runs the NimBLE event loop (blocks until stack shutdown)
    nimble_port_run();

    nimble_port_freertos_deinit();
}

static void ble_on_reset(int reason){
    ESP_LOGE(TAG, "BLE Reset. Reason = %d", reason);
}

static void ble_on_sync(void){
    int rc;

    ESP_LOGI(TAG, "BLE Stack Synchronized");

    rc = ble_hs_util_ensure_addr(0);
    ESP_ERROR_CHECK(rc);

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    ESP_LOGI(TAG, "Own address type = %d", own_addr_type);
    ESP_ERROR_CHECK(rc);

    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    ble_start_advertising();
}

static ble_connection_t *ble_find_connection(uint16_t conn_handle)
{
    return connection.conn_handle == conn_handle ? &connection : NULL;
}

static bool ble_add_connection(uint16_t conn_handle)
{
    if (connection.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }

    connection.conn_handle = conn_handle;
    connection.encrypted = false;
    return true;
}

static void ble_remove_connection(uint16_t conn_handle)
{
    ble_connection_t *connection = ble_find_connection(conn_handle);
    if (connection != NULL) {
        connection->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        connection->encrypted = false;
    }
}

static int ble_gap_event(struct ble_gap_event *event, void *arg){
    switch (event->type){

        case BLE_GAP_EVENT_CONNECT:{
            if (event->connect.status == 0){
                if (!ble_add_connection(event->connect.conn_handle)) {
                    ESP_LOGW(TAG, "Connection limit reached");
                    ble_gap_terminate(event->connect.conn_handle,
                                      BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                ESP_LOGI(TAG, "Connected");

                int rc = ble_gap_security_initiate(event->connect.conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGE(TAG, "Failed to initiate pairing (%d)", rc);
                }

            } else {
                ESP_LOGW(TAG, "Connection attempt failed (%d)", event->connect.status);
                ble_start_advertising();
            }
            break;
        }

        /*case BLE_GAP_EVENT_ENC_CHANGE:{
            if (event->enc_change.status == 0) {
                link_encrypted = true;
                ESP_LOGI(TAG, "BLE link encrypted");
            } else {
                link_encrypted = false;
                ESP_LOGE(TAG, "Pairing/encryption failed (%d)",
                         event->enc_change.status);
            }
            break;
        }*/

        case BLE_GAP_EVENT_ENC_CHANGE:{
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);

            if (event->enc_change.status == 0 && rc == 0 && desc.sec_state.encrypted){
                ble_connection_t *connection =
                    ble_find_connection(event->enc_change.conn_handle);
                if (connection != NULL) {
                    connection->encrypted = true;
                }
                ESP_LOGI(TAG, "Encrypted=%d Bonded=%d Authenticated=%d",
                        desc.sec_state.encrypted, desc.sec_state.bonded,
                        desc.sec_state.authenticated);
                ESP_LOGI(TAG, "BLE link encrypted");
            }
            else{
                ble_connection_t *connection =
                    ble_find_connection(event->enc_change.conn_handle);
                if (connection != NULL) {
                    connection->encrypted = false;
                }
                ESP_LOGE(TAG, "Pairing/encryption failed (status=%d, desc=%d)",
                         event->enc_change.status, rc);
            }
            break;
        }

        case BLE_GAP_EVENT_DISCONNECT:{
            ble_hid_handle_disconnect(event->disconnect.conn.conn_handle);
            ble_ota_handle_disconnect(event->disconnect.conn.conn_handle);
            ble_remove_connection(event->disconnect.conn.conn_handle);
            ESP_LOGI(TAG, "Disconnected (reason = %d)", event->disconnect.reason);
            ble_start_advertising();
            break;
        }

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc desc;
            int rc;

            rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            if (rc == 0) {
                /*
                 * Compatibility behavior until the hardware has a local
                 * pairing-window / factory-reset flow.  That future flow
                 * must gate bond replacement on explicit user consent.
                 */
                ESP_LOGW(TAG, "Repeat pairing requested; replacing old bond");
                ble_store_util_delete_peer(&desc.peer_id_addr);
                return BLE_GAP_REPEAT_PAIRING_RETRY;
            }

            ESP_LOGE(TAG, "Repeat pairing lookup failed (%d)", rc);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }

        case BLE_GAP_EVENT_SUBSCRIBE:{
            ble_hid_handle_subscribe(event);
            break;
        }

        case BLE_GAP_EVENT_PASSKEY_ACTION:{
            ESP_LOGI(TAG, "PASSKEY ACTION = %d", event->passkey.params.action);
            return 0;
        }
    }
    return 0;
}

//-----------BLE ADVERTISING---------------
static void ble_start_advertising(void){
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));

    if (ble_transport_connected()) {
        ESP_LOGI(TAG, "Advertising paused while the device is connected");
        return;
    }

    if (ble_gap_adv_active()) {
        ESP_LOGI(TAG, "Advertising already active");
        return;
    }

    int rc;
    //static const ble_uuid16_t adv_uuids[] = {
    //    BLE_UUID16_INIT(HID_SERVICE_UUID),
    //};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = BLE_DEVICE_NAME;
    fields.appearance = 961;      // Keyboard
    fields.appearance_is_present = 1;

    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    static const ble_uuid16_t adv_svcs[] = {
        BLE_UUID16_INIT(HID_SERVICE_UUID),
        BLE_UUID16_INIT(OTA_SERVICE_UUID),
    };

    fields.uuids16 = adv_svcs;
    fields.num_uuids16 = sizeof(adv_svcs) / sizeof(adv_svcs[0]);
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);

    ESP_LOGI(TAG, "Name: %s", name);
    ESP_LOGI(TAG, "Name length: %d", fields.name_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertising fields (%d)", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(
            own_addr_type,
            NULL,
            BLE_HS_FOREVER,
            &adv_params,
            ble_gap_event,
            NULL);

    ESP_LOGI(TAG, "ble_gap_adv_start() = %d", rc);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising (%d)", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising started");
}


//-----------PUBLIC FUNCTIONS--------------

void ble_transport_init(void){

    connection.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    connection.encrypted = false;

    ble_init_nvs();
    ble_init_host();
    ble_init_security();
    ble_init_services();
    ble_device_info_init();

    ble_gatt_init();

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb  = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
}

//----------Helpers------------
bool ble_transport_connected(void){
    return connection.conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_transport_link_encrypted(uint16_t conn_handle){
    ble_connection_t *connection = ble_find_connection(conn_handle);
    return connection != NULL && connection->encrypted;
}
