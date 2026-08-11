#include "ble_device_info.h"
#include "ble_uuid.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

static const char *TAG = "BLE_INFO";

static const char manufacturer_name[] = "GAMR";
static const char model_number[] = "GAMR 3x3";
static const char hardware_revision[] = "ESP32-C6";
static const char firmware_revision[] = "0.1.0";
static char device_id[] = "GAMR-000000000000";

static int device_info_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *value = NULL;

    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (ble_uuid_u16(ctxt->chr->uuid)) {
        case DEVICE_INFO_MANUFACTURER_NAME_UUID:
            value = manufacturer_name;
            break;
        case DEVICE_INFO_MODEL_NUMBER_UUID:
            value = model_number;
            break;
        case DEVICE_INFO_SERIAL_NUMBER_UUID:
            value = device_id;
            break;
        case DEVICE_INFO_HARDWARE_REVISION_UUID:
            value = hardware_revision;
            break;
        case DEVICE_INFO_FIRMWARE_REVISION_UUID:
            value = firmware_revision;
            break;
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }

    return os_mbuf_append(ctxt->om, value, strlen(value)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

const struct ble_gatt_svc_def device_info_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(DEVICE_INFO_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(DEVICE_INFO_MANUFACTURER_NAME_UUID),
                .access_cb = device_info_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(DEVICE_INFO_MODEL_NUMBER_UUID),
                .access_cb = device_info_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(DEVICE_INFO_SERIAL_NUMBER_UUID),
                .access_cb = device_info_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(DEVICE_INFO_HARDWARE_REVISION_UUID),
                .access_cb = device_info_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(DEVICE_INFO_FIRMWARE_REVISION_UUID),
                .access_cb = device_info_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

void ble_device_info_init(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BT);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read Bluetooth MAC: %s", esp_err_to_name(err));
        return;
    }

    snprintf(device_id, sizeof(device_id), "GAMR-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device ID: %s", device_id);
}
