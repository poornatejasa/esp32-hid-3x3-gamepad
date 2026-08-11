#pragma once

#include "host/ble_gatt.h"

/*
 * Standard Bluetooth Device Information service. The serial-number
 * characteristic is this product's stable device ID.
 */
void ble_device_info_init(void);

extern const struct ble_gatt_svc_def device_info_gatt_svcs[];
