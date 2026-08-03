#pragma once

#include "host/ble_gatt.h"

// OTA Commands
typedef enum{
    OTA_CMD_START  = 0x01,
    OTA_CMD_END    = 0x02,
    OTA_CMD_ABORT  = 0x03,
    OTA_CMD_REBOOT = 0x04
} ota_command_t;

// START packet
typedef struct __attribute__((packed)){
    uint8_t command;
    uint32_t image_size;
} ota_start_packet_t;

//STATUS Packet
typedef struct __attribute__((packed)){
    uint8_t state;
    uint8_t progress;
    uint32_t bytes_received;
    uint32_t total_size;
} ota_status_packet_t;

// GATT Service
extern const struct ble_gatt_svc_def ota_gatt_svcs[];