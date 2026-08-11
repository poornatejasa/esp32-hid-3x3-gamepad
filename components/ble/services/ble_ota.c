#include "ble_ota.h"
#include "ble_uuid.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "ota.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BLE_OTA";

static uint16_t ota_control_handle;
static uint16_t ota_data_handle;
static uint16_t ota_status_handle;
static uint16_t ota_owner_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t last_progress;

static int handle_control(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt);
static int handle_data(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt);
static int handle_status(struct ble_gatt_access_ctxt *ctxt);
static int ble_ota_copy_data(struct ble_gatt_access_ctxt *ctxt, void *dest, size_t len);
static int ble_ota_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ota_restart_task(void *arg);

//--------------------GATT Service Variable----------------------
const struct ble_gatt_svc_def ota_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(OTA_SERVICE_UUID),

        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                .uuid = BLE_UUID16_DECLARE(OTA_CONTROL_UUID),
                .access_cb = ble_ota_access_cb,
                .val_handle = &ota_control_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },

            {
                .uuid = BLE_UUID16_DECLARE(OTA_DATA_UUID),
                .access_cb = ble_ota_access_cb,
                .val_handle = &ota_data_handle,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },

            {
                .uuid = BLE_UUID16_DECLARE(OTA_STATUS_UUID),
                .access_cb = ble_ota_access_cb,
                .val_handle = &ota_status_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC
                            | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
            },

            {0}
        },
    },

    {0}
};

static int ble_ota_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg){
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op){

        case BLE_GATT_ACCESS_OP_WRITE_CHR:{
            switch (ble_uuid_u16(ctxt->chr->uuid)){

                case OTA_CONTROL_UUID:{
                    return handle_control(conn_handle, ctxt);
                }
                
                case OTA_DATA_UUID:{
                    return handle_data(conn_handle, ctxt);
                }

                default:
                    return BLE_ATT_ERR_UNLIKELY;
            }
        }

        case BLE_GATT_ACCESS_OP_READ_CHR:{
            switch (ble_uuid_u16(ctxt->chr->uuid)){

                case OTA_STATUS_UUID:{
                    return handle_status(ctxt);
                }

                default:
                    return BLE_ATT_ERR_UNLIKELY;
            }
        }

        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

static int handle_control(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt){
    if (OS_MBUF_PKTLEN(ctxt->om) < 1)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t cmd;
    int rc = ble_ota_copy_data(ctxt, &cmd, sizeof(cmd));
    if (rc != 0)
        return rc;

    switch (cmd){
        case OTA_CMD_ENTER:{
            ESP_LOGI(TAG, "OTA service is already available");
            return 0;
        }
        case OTA_CMD_START:{
            if (ota_owner_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
                ota_owner_conn_handle != conn_handle) {
                return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            }
            ESP_LOGI(TAG,
            "Received START packet: %u bytes, Expected: %u bytes",
            (unsigned)OS_MBUF_PKTLEN(ctxt->om),
            (unsigned)sizeof(ota_start_packet_t));
            if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(ota_start_packet_t)){
                ESP_LOGE(TAG, "Invalid START packet");
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            ota_start_packet_t start_packet;
            int rc = ble_ota_copy_data(ctxt, &start_packet, sizeof(start_packet));
            if (rc != 0)
                return rc;
                                        
            esp_err_t err = ota_begin(start_packet.image_size,
                                      start_packet.crc32);
            last_progress = 0;
            if (err != ESP_OK){
                ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
                return BLE_ATT_ERR_UNLIKELY;
            }
            ota_owner_conn_handle = conn_handle;
            ESP_LOGI(TAG, "OTA Started (%u bytes)", (unsigned)start_packet.image_size);
            return 0;
        }

        case OTA_CMD_END:{
            if (ota_owner_conn_handle != conn_handle) {
                return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            }
            esp_err_t err = ota_finish();
            if (err != ESP_OK){
                ESP_LOGE(TAG, "ota_finish failed: %s", esp_err_to_name(err));
                return BLE_ATT_ERR_UNLIKELY;
            }
            ESP_LOGI(TAG, "OTA Finished");
            return 0;
        }
        
        case OTA_CMD_ABORT:{
            if (ota_owner_conn_handle != conn_handle) {
                return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            }
            ota_abort();
            ota_owner_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            last_progress = 0;
            ESP_LOGI(TAG, "OTA Aborted");
            return 0;
        }

        case OTA_CMD_REBOOT:{
            if (ota_owner_conn_handle != conn_handle ||
                ota_get_state() != OTA_STATE_COMPLETED) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            if (xTaskCreate(ota_restart_task, "ota_restart", 2048, NULL,
                            tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
                ESP_LOGE(TAG, "Failed to schedule OTA reboot");
                return BLE_ATT_ERR_UNLIKELY;
            }
            ESP_LOGI(TAG, "OTA Reboot");
            return 0;
        }

        default:{
            ESP_LOGW(TAG, "Unknown OTA command: 0x%02X", cmd);
            return BLE_ATT_ERR_UNLIKELY;
        }
    }
}

static int handle_data(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt){
    if (ota_owner_conn_handle != conn_handle) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);

    static uint8_t buffer[512];
    if (len > sizeof(buffer)){
        ESP_LOGE(TAG, "OTA packet too large");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    int rc = ble_ota_copy_data(ctxt, buffer, len);
    if (rc != 0)
        return rc;

    esp_err_t err = ota_write(buffer, len);
    if (err != ESP_OK){
        ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t progress = ota_get_progress();
    if (progress >= last_progress + 5){
        last_progress = progress;
        ESP_LOGI(TAG, "Progress: %u%%", progress);
    }
    return 0;
}

static int handle_status(struct ble_gatt_access_ctxt *ctxt){
    ota_status_packet_t status ={
        .state = ota_get_state(),
        .progress = ota_get_progress(),
        .bytes_received = ota_get_bytes_received(),
        .total_size = ota_get_total_size()
    };
    if (os_mbuf_append(ctxt->om, &status, sizeof(status)) != 0)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    return 0;
}

static int ble_ota_copy_data(struct ble_gatt_access_ctxt *ctxt, void *dest, size_t len){
    if (OS_MBUF_PKTLEN(ctxt->om) < len){
        ESP_LOGE(TAG, "Invalid packet length");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    int rc = os_mbuf_copydata(ctxt->om, 0, len, dest);
    if (rc != 0){
        ESP_LOGE(TAG, "Failed to copy packet");
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

static void ota_restart_task(void *arg)
{
    (void)arg;

    // Return the GATT write response before disconnecting the central.
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

void ble_ota_handle_disconnect(uint16_t conn_handle)
{
    if (ota_owner_conn_handle == conn_handle) {
        ESP_LOGW(TAG, "OTA owner disconnected; aborting OTA session");
        ota_abort();
        ota_owner_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        last_progress = 0;
    }
}
