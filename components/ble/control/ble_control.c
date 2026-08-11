#include "ble_control.h"

#include "ble_uuid.h"

#include "esp_log.h"

static const char *TAG = "BLE_CTRL";

static uint16_t control_handle;

static int control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

const struct ble_gatt_svc_def control_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(CONTROL_SERVICE_UUID),

        .characteristics =
        (struct ble_gatt_chr_def[])
        {
            {
                .uuid = BLE_UUID16_DECLARE(CONTROL_CHARACTERISTIC_UUID),
                .access_cb = control_access_cb,
                .val_handle = &control_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },

            {0}
        },
    },

    {0}
};

static int control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg){
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (OS_MBUF_PKTLEN(ctxt->om) != 1)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t cmd;

    os_mbuf_copydata(ctxt->om,0,1,&cmd);

    if(cmd == 0x01)
    {
        /* OTA is always available; this command is now a no-reboot request. */
        ESP_LOGI(TAG,"OTA session requested");
    }

    return 0;
}
