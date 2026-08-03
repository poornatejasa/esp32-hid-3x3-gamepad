#include "ble_uuid.h"
#include "ble_hid.h"
#include "ble_transport.h"
#include "hid_core.h"

#include <stdint.h>
#include <stdbool.h>

#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"
#include "esp_log.h"

static const char *TAG = "BLE_HID";

static uint16_t hid_input_report_handle;
static uint16_t hid_output_report_handle;
static uint16_t boot_keyboard_input_handle;

static bool report_notifications_enabled;
static bool boot_notifications_enabled;

static int ble_hid_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);

//--------------------GATT Service Variable----------------------

const struct ble_gatt_svc_def hid_gatt_svcs[] ={
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(HID_SERVICE_UUID),

        .characteristics = (struct ble_gatt_chr_def[])
        {
            // HID Information
            {
                  .uuid = BLE_UUID16_DECLARE(HID_INFORMATION_UUID),
                .access_cb = ble_hid_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },  

            // Report Map
            {
                .uuid = BLE_UUID16_DECLARE(HID_REPORT_MAP_UUID),
                .access_cb = ble_hid_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            // Protocol Mode
            {
                .uuid = BLE_UUID16_DECLARE(HID_PROTOCOL_MODE_UUID),
                .access_cb = ble_hid_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
            },
            // Input Report
            {
                .uuid = BLE_UUID16_DECLARE(HID_REPORT_UUID),
                .access_cb = ble_hid_access_cb,
                .val_handle = &hid_input_report_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .descriptors = (struct ble_gatt_dsc_def[])
                {
                    {
                        .uuid = BLE_UUID16_DECLARE(REPORT_REFERENCE_UUID),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                        .access_cb = ble_hid_access_cb,
                    },
                    {0}
                },
            },
            // Boot Keyboard Input Report.  Hosts that choose Boot protocol
            // read/subscribe to this report instead of the Report-protocol
            // characteristic above.
            {
                .uuid = BLE_UUID16_DECLARE(BOOT_KEYBOARD_INPUT_UUID),
                .access_cb = ble_hid_access_cb,
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
                .access_cb = ble_hid_access_cb,
                .val_handle = &hid_output_report_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
                .descriptors = (struct ble_gatt_dsc_def[])
                {
                    {
                        .uuid = BLE_UUID16_DECLARE(REPORT_REFERENCE_UUID),
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                        .access_cb = ble_hid_access_cb,
                    },
                    {0}
                },
            },
            // Boot Keyboard Output Report mirrors the LED state for hosts using
            // Boot protocol.
            {
                .uuid = BLE_UUID16_DECLARE(BOOT_KEYBOARD_OUTPUT_UUID),
                .access_cb = ble_hid_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
            },
            // HID Control Point
            {
                .uuid = BLE_UUID16_DECLARE(HID_CONTROL_POINT_UUID),
                .access_cb = ble_hid_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },

            {0}
        },
    },

    {0}
};

//-----------------GATT--------------------
static int ble_hid_access_cb(uint16_t conn_handle, uint16_t attr_handle,
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

//--------------Public API-----------------
void ble_hid_handle_subscribe(const struct ble_gap_event *event){
    if (event->subscribe.attr_handle == hid_input_report_handle){
        report_notifications_enabled = event->subscribe.cur_notify;

        ESP_LOGI(TAG, "Report notifications %s",
                 report_notifications_enabled ? "Enabled" : "Disabled");
    }
    else if (event->subscribe.attr_handle == boot_keyboard_input_handle){
        boot_notifications_enabled = event->subscribe.cur_notify;

        ESP_LOGI(TAG,"Boot notifications %s",
                 boot_notifications_enabled ? "Enabled" : "Disabled");
    }
}

void ble_hid_reset(void){
    report_notifications_enabled = false;
    boot_notifications_enabled = false;
}

esp_err_t ble_hid_send_report(const uint8_t *report, size_t len){ 
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

    if (!ble_transport_link_encrypted()) {
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

    int rc = ble_gatts_notify_custom(ble_transport_conn_handle(), report_handle, om);

    if (rc != 0){
        ESP_LOGE(TAG, "Notify failed (%d)", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "HID report sent");
    ESP_LOGI(TAG, "Sending %u bytes", (unsigned)len);
    return ESP_OK;
}