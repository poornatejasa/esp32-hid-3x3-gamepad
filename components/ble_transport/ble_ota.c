#include "ble_ota.h"
#include "ble_uuid.h"
#include "host/ble_gatt.h"
#include "ota.h"

#include "esp_log.h"

static const char *TAG = "BLE_OTA";

static uint16_t ota_control_handle;
static uint16_t ota_data_handle;
static uint16_t ota_status_handle;
static uint8_t last_progress;

static int handle_control(struct ble_gatt_access_ctxt *ctxt);
static int handle_data(struct ble_gatt_access_ctxt *ctxt);
static int handle_status(struct ble_gatt_access_ctxt *ctxt);
static int ble_ota_copy_data(struct ble_gatt_access_ctxt *ctxt, void *dest, size_t len);
static int ble_ota_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

//--------------------GATT Service Variable----------------------
const struct ble_gatt_svc_def ota_gatt_svcs[] =
{
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(OTA_SERVICE_UUID),

        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                .uuid = BLE_UUID16_DECLARE(OTA_CONTROL_UUID),
                .access_cb = ble_ota_access_cb,
                .val_handle = &ota_control_handle,
                .flags = BLE_GATT_CHR_F_WRITE,
            },

            {
                .uuid = BLE_UUID16_DECLARE(OTA_DATA_UUID),
                .access_cb = ble_ota_access_cb,
                .val_handle = &ota_data_handle,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },

            {
                .uuid = BLE_UUID16_DECLARE(OTA_STATUS_UUID),
                .access_cb = ble_ota_access_cb,
                .val_handle = &ota_status_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },

            {0}
        },
    },

    {0}
};

static int ble_ota_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg){
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op){

        case BLE_GATT_ACCESS_OP_WRITE_CHR:{
            switch (ble_uuid_u16(ctxt->chr->uuid)){

                case OTA_CONTROL_UUID:{
                    return handle_control(ctxt);
                }
                
                case OTA_DATA_UUID:{
                    return handle_data(ctxt);
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

static int handle_control(struct ble_gatt_access_ctxt *ctxt){
    if (OS_MBUF_PKTLEN(ctxt->om) < 1)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t cmd;
    int rc = ble_ota_copy_data(ctxt, &cmd, sizeof(cmd));
    if (rc != 0)
        return rc;

    switch (cmd){
        case OTA_CMD_START:{
            if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(ota_start_packet_t)){
                ESP_LOGE(TAG, "Invalid START packet");
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            ota_start_packet_t start_packet;
            int rc = ble_ota_copy_data(ctxt, &start_packet, sizeof(start_packet));
            if (rc != 0)
                return rc;
                                        
            esp_err_t err = ota_begin(start_packet.image_size);
            last_progress = 0;
            if (err != ESP_OK){
                ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
                return BLE_ATT_ERR_UNLIKELY;
            }
            ESP_LOGI(TAG, "OTA Started (%u bytes)", (unsigned)start_packet.image_size);
            return 0;
        }

        case OTA_CMD_END:{
            esp_err_t err = ota_finish();
            if (err != ESP_OK){
                ESP_LOGE(TAG, "ota_finish failed: %s", esp_err_to_name(err));
                return BLE_ATT_ERR_UNLIKELY;
            }
            ESP_LOGI(TAG, "OTA Finished");
            return 0;
        }
        
        case OTA_CMD_ABORT:{
            ota_abort();
            last_progress = 0;
            ESP_LOGI(TAG, "OTA Aborted");
            return 0;
        }

        case OTA_CMD_REBOOT:{
            ota_reboot();
            ESP_LOGI(TAG, "OTA Reboot");
            return 0;
        }

        default:{
            ESP_LOGW(TAG, "Unknown OTA command: 0x%02X", cmd);
            return BLE_ATT_ERR_UNLIKELY;
        }
    }
}

static int handle_data(struct ble_gatt_access_ctxt *ctxt){
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