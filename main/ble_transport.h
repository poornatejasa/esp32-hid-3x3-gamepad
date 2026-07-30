#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void ble_transport_init(void);

bool ble_transport_connected(void);

esp_err_t ble_transport_send_report(const uint8_t *report, size_t len);