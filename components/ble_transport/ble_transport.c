#include "ble_transport.h"
#include "hid_core.h"

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

#define BLE_DEVICE_NAME    "Poorna_GAMR"

//---------------------GATT UUIDs--------------------------------

#define GAMR_SERVICE_UUID        0xFFF0
#define GAMR_CHARACTERISTIC_UUID 0xFFF1

//---------------------HID UUIDs---------------------------------

#define HID_SERVICE_UUID             0x1812

#define HID_INFORMATION_UUID         0x2A4A
#define HID_REPORT_MAP_UUID          0x2A4B
#define HID_CONTROL_POINT_UUID       0x2A4C
#define HID_REPORT_UUID              0x2A4D
#define HID_PROTOCOL_MODE_UUID       0x2A4E
#define BOOT_KEYBOARD_INPUT_UUID      0x2A22
#define BOOT_KEYBOARD_OUTPUT_UUID     0x2A32

#define REPORT_REFERENCE_UUID        0x2908

//--------------------------Variables-----------------------------

static const char *TAG = "BLE";
static uint8_t own_addr_type;

static uint16_t hid_input_report_handle;
static uint16_t hid_output_report_handle;
static uint16_t boot_keyboard_input_handle;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool report_notifications_enabled;
static bool boot_notifications_enabled;
static bool link_encrypted;

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
 
static int ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);
  
//--------------------GATT Service Variable----------------------

static const struct ble_gatt_svc_def gatt_svcs[] ={
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(HID_SERVICE_UUID),

        .characteristics = (struct ble_gatt_chr_def[])
        {
            // HID Information
            {
                  .uuid = BLE_UUID16_DECLARE(HID_INFORMATION_UUID),
                .access_cb = ble_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },  

            // Report Map
            {
                .uuid = BLE_UUID16_DECLARE(HID_REPORT_MAP_UUID),
                .access_cb = ble_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            // Protocol Mode
            {
                .uuid = BLE_UUID16_DECLARE(HID_PROTOCOL_MODE_UUID),
                .access_cb = ble_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
            },
            // Input Report
            {
                .uuid = BLE_UUID16_DECLARE(HID_REPORT_UUID),
                .access_cb = ble_gatt_access_cb,
                .val_handle = &hid_input_report_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .descriptors = (struct ble_gatt_dsc_def[])
                {
                    {
                        .uuid = BLE_UUID16_DECLARE(REPORT_REFERENCE_UUID),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                        .access_cb = ble_gatt_access_cb,
                    },
                    {0}
                },
            },
            // Boot Keyboard Input Report.  Hosts that choose Boot protocol
            // read/subscribe to this report instead of the Report-protocol
            // characteristic above.
            {
                .uuid = BLE_UUID16_DECLARE(BOOT_KEYBOARD_INPUT_UUID),
                .access_cb = ble_gatt_access_cb,
                .val_handle = &boot_keyboard_input_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
            },
            // Output Report for keyboard LED state (Num Lock, Caps Lock, etc.).
            // Some hosts, especially macOS, expect a keyboard to expose this
            // report even if the device itself ignores the LED state.
            {
                .uuid = BLE_UUID16_DECLARE(HID_REPORT_UUID),
                .access_cb = ble_gatt_access_cb,
                .val_handle = &hid_output_report_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
                .descriptors = (struct ble_gatt_dsc_def[])
                {
                    {
                        .uuid = BLE_UUID16_DECLARE(REPORT_REFERENCE_UUID),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                        .access_cb = ble_gatt_access_cb,
                    },
                    {0}
                },
            },
            // Boot Keyboard Output Report mirrors the LED state for hosts using
            // Boot protocol.
            {
                .uuid = BLE_UUID16_DECLARE(BOOT_KEYBOARD_OUTPUT_UUID),
                .access_cb = ble_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
            },
            // HID Control Point
            {
                .uuid = BLE_UUID16_DECLARE(HID_CONTROL_POINT_UUID),
                .access_cb = ble_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },

            {0}
        },
    },

    {0}
};

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
    // Windows expects a BLE keyboard to use a bonded, encrypted connection.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_store_config_init();

    ESP_LOGI(TAG, "BLE security and bond store configured");
}

static void ble_init_services(void){
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ESP_LOGI(TAG, "GAP/GATT services initialized");
}

static void ble_gatt_init(void){
    int rc;

    rc = ble_gatts_count_cfg(gatt_svcs);
    assert(rc == 0);

    rc = ble_gatts_add_svcs(gatt_svcs);
    assert(rc == 0);

    ESP_LOGI(TAG, "GATT database registered");
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

static int ble_gap_event(struct ble_gap_event *event, void *arg){
    switch (event->type){
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0){
                int rc;

                conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "Connected");

                rc = ble_gap_security_initiate(conn_handle);
                ESP_LOGI(TAG, "ble_gap_security_initiate() = %d", rc);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGE(TAG, "Failed to initiate pairing (%d)", rc);
                }
            } else {
                ESP_LOGW(TAG, "Connection attempt failed (%d)", event->connect.status);
                ble_start_advertising();
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == 0) {
                link_encrypted = true;
                ESP_LOGI(TAG, "BLE link encrypted");
            } else {
                link_encrypted = false;
                ESP_LOGE(TAG, "Pairing/encryption failed (%d)",
                         event->enc_change.status);
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            report_notifications_enabled = false;
            boot_notifications_enabled = false;
            link_encrypted = false;
            ESP_LOGI(TAG, "Disconnected (reason = %d)", event->disconnect.reason);
            ble_start_advertising();
            break;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc desc;
            int rc;

            rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGW(TAG, "Repeat pairing requested; deleting old bond");
                ble_store_util_delete_peer(&desc.peer_id_addr);
                return BLE_GAP_REPEAT_PAIRING_RETRY;
            }

            ESP_LOGE(TAG, "Repeat pairing lookup failed (%d)", rc);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == hid_input_report_handle) {
                report_notifications_enabled = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "Report-protocol notifications %s",
                         report_notifications_enabled ? "Enabled" : "Disabled");
            } else if (event->subscribe.attr_handle == boot_keyboard_input_handle) {
                boot_notifications_enabled = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "Boot-protocol notifications %s",
                         boot_notifications_enabled ? "Enabled" : "Disabled");
            }
            break;
    }
    return 0;
}

//-----------BLE ADVERTISING---------------
static void ble_start_advertising(void){
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;

    memset(&fields, 0, sizeof(fields));

    int rc;
    //static const ble_uuid16_t adv_uuids[] = {
    //    BLE_UUID16_INIT(HID_SERVICE_UUID),
    //};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = BLE_DEVICE_NAME;

    fields.name = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    
    fields.appearance = 961; // Generic Keyboard
    fields.appearance_is_present = 1;

    static const ble_uuid16_t adv_svcs[] = {
        BLE_UUID16_INIT(HID_SERVICE_UUID)
    };

    fields.uuids16 = adv_svcs;
    fields.num_uuids16 = 1;
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

//-----------------GATT--------------------
static int ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const hid_gamepad_report_t *report = hid_get_report();

    // Report Reference (0x2908) is a GATT descriptor, not a characteristic.
    // Windows reads it during HID-over-GATT discovery to associate this
    // characteristic with report ID 1 and the Input report type.
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        if (ble_uuid_u16(ctxt->dsc->uuid) == REPORT_REFERENCE_UUID) {
            if (attr_handle == hid_output_report_handle + 1) {
                os_mbuf_append(ctxt->om, hid_output_report_reference, hid_output_report_reference_len);
                ESP_LOGI(TAG, "Output Report Reference Read");
            } else {
                os_mbuf_append(ctxt->om, hid_report_reference, hid_report_reference_len);
                ESP_LOGI(TAG, "Input Report Reference Read");
            }
            return 0;
        }

        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (ctxt->op){
        case BLE_GATT_ACCESS_OP_READ_CHR:
            switch (ble_uuid_u16(ctxt->chr->uuid)){
                case HID_INFORMATION_UUID:{
                    os_mbuf_append(ctxt->om, hid_info, hid_info_len);
                    ESP_LOGI(TAG, "HID Information Read");
                    return 0;
                }

                case HID_REPORT_MAP_UUID:{
                    os_mbuf_append(ctxt->om, hid_report_map, hid_report_map_len);
                    ESP_LOGI(TAG, "HID Report Map Read");
                    return 0;
                }

                case HID_PROTOCOL_MODE_UUID:{
                    uint8_t protocol = hid_get_protocol_mode();
                    os_mbuf_append(ctxt->om, &protocol, sizeof(protocol));
                    ESP_LOGI(TAG, "Protocol Mode Read");
                    return 0;
                }

                case HID_REPORT_UUID:{
                    if (attr_handle == hid_output_report_handle) {
                        uint8_t led = hid_get_led_state();
                        os_mbuf_append(ctxt->om, &led, sizeof(led));
                        ESP_LOGI(TAG, "Output Report Read");
                    } else {
                        os_mbuf_append(ctxt->om, report, sizeof(*report));
                        ESP_LOGI(TAG, "Input Report Read");
                    }
                    return 0;
                }

                case BOOT_KEYBOARD_INPUT_UUID:{
                    os_mbuf_append(ctxt->om, report, sizeof(*report));
                    ESP_LOGI(TAG, "Boot Keyboard Input Report Read");
                    return 0;
                }

                case BOOT_KEYBOARD_OUTPUT_UUID:{
                    uint8_t led = hid_get_led_state();
                    os_mbuf_append(ctxt->om, &led, sizeof(led));
                    ESP_LOGI(TAG, "Boot Keyboard Output Report Read");
                    return 0;
                }

                default:
                    return BLE_ATT_ERR_UNLIKELY;
            }

        case BLE_GATT_ACCESS_OP_WRITE_CHR:
            if (OS_MBUF_PKTLEN(ctxt->om) < 1) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            switch (ble_uuid_u16(ctxt->chr->uuid)){
                case HID_PROTOCOL_MODE_UUID: {
                    uint8_t requested_mode = ctxt->om->om_data[0];
                    if (requested_mode > 1) {
                        return BLE_ATT_ERR_UNLIKELY;
                    }
                    hid_set_protocol_mode(requested_mode);
                    ESP_LOGI(TAG, "Protocol Mode = %d", hid_get_protocol_mode());
                    return 0;
                }

                case HID_CONTROL_POINT_UUID:{
                    hid_set_control_point(ctxt->om->om_data[0]);
                    ESP_LOGI(TAG, "HID Control Point = %d", hid_get_control_point());

                    return 0;
                }

                case HID_REPORT_UUID:
                
                case BOOT_KEYBOARD_OUTPUT_UUID:{
                    hid_set_led_state(ctxt->om->om_data[0]);
                    ESP_LOGI(TAG, "Keyboard LED state = 0x%02X", hid_get_led_state());
                    return 0;
                }

                default:
                    return BLE_ATT_ERR_UNLIKELY;
            }
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

//-----------PUBLIC FUNCTIONS--------------

void ble_transport_init(void){
    ble_init_nvs();
    ble_init_host();
    ble_init_security();
    ble_init_services();

    ble_gatt_init();

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb  = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
}

bool ble_transport_connected(void){
    return (conn_handle != BLE_HS_CONN_HANDLE_NONE);
}

esp_err_t ble_transport_send_report(const uint8_t *report, size_t len){ 
    struct os_mbuf *om;
    uint16_t report_handle = hid_input_report_handle;
    bool notifications_enabled = report_notifications_enabled;

    // The Report Reference descriptor provides Report ID 1 for the Report
    // characteristic. HID-over-GATT values therefore contain only the report
    // payload, never a Report ID byte.
    if (hid_get_protocol_mode() == 0) {
        report_handle = boot_keyboard_input_handle;
        notifications_enabled = boot_notifications_enabled;
    }

    if (!ble_transport_connected()){
        ESP_LOGW(TAG, "No BLE connection");
        return ESP_FAIL;
    }

    if (!link_encrypted) {
        ESP_LOGW(TAG, "BLE link is not encrypted yet");
        return ESP_ERR_INVALID_STATE;
    }

    if (!notifications_enabled) {
        ESP_LOGW(TAG, "%s-protocol notifications are not enabled",
                 hid_get_protocol_mode() == 0 ? "Boot" : "Report");
        return ESP_ERR_INVALID_STATE;
    }
    om = ble_hs_mbuf_from_flat(report, len);

    if (om == NULL){
        ESP_LOGE(TAG, "Failed to allocate mbuf");
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(conn_handle, report_handle, om);

    if (rc != 0){
        ESP_LOGE(TAG, "Notify failed (%d)", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "HID report sent");
    ESP_LOGI(TAG, "Sending %u bytes", (unsigned)len);
    return ESP_OK;
}