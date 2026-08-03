#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
                              
extern const struct ble_gatt_svc_def hid_gatt_svcs[];

extern void ble_hid_handle_subscribe(const struct ble_gap_event *event);
void ble_hid_reset(void);
esp_err_t ble_hid_send_report(const uint8_t *report, size_t len);