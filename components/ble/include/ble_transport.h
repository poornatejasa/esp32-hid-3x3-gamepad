#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void ble_transport_init(void);

bool ble_transport_connected(void);
bool ble_transport_link_encrypted(uint16_t conn_handle);
