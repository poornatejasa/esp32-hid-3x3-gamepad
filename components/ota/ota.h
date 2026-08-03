#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum{
    OTA_STATE_IDLE = 0,
    OTA_STATE_STARTED,
    OTA_STATE_RECEIVING,
    OTA_STATE_VERIFYING,
    OTA_STATE_COMPLETED,
    OTA_STATE_FAILED
} ota_state_t;

// ---------------------Function Prototypes-----------------------
void ota_init(void);
void ota_reboot(void);
void ota_reset(void);
void ota_abort(void);

ota_state_t ota_get_state(void);
size_t ota_get_bytes_received(void);
size_t ota_get_total_size(void);
uint8_t ota_get_progress(void);

esp_err_t ota_begin(size_t firmware_size);
esp_err_t ota_write(const uint8_t *data, size_t length);
esp_err_t ota_finish(void);