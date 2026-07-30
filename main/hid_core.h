#pragma once

#include <stdint.h>
#include "matrix.h"

typedef struct __attribute__((packed)){
    uint8_t modifiers;       // Left/Right Ctrl, Shift, Alt, GUI
    uint8_t reserved;
    uint8_t keycodes[6];     // Standard six-key rollover keyboard report

} hid_gamepad_report_t;

typedef enum{
    HID_BTN_CENTER = 0,
    HID_BTN_UP,
    HID_BTN_DOWN,
    HID_BTN_LEFT,
    HID_BTN_RIGHT,
    HID_BTN_A,
    HID_BTN_B,
    HID_BTN_X,
    HID_BTN_Y
} hid_button_t;

void hid_core_init(void);

void hid_core_press(matrix_key_t key);
void hid_core_release(matrix_key_t key);

const hid_gamepad_report_t *hid_get_report(void);