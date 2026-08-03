#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

extern const struct ble_gatt_svc_def ota_gatt_svcs[];

void ble_ota_init(void);
void ble_ota_reset(void);