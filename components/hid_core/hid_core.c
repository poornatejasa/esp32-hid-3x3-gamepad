#include "hid_core.h"

#include <stdio.h>
#include <string.h>

//--------------------------Variables-----------------------------

static hid_gamepad_report_t report;

static uint8_t hid_protocol_mode = 0x01;
static uint8_t hid_control_point = 0;
static uint8_t keyboard_led_state = 0;

//---------------------- Global Variables-------------------------

const uint8_t hid_info[] ={
    0x11, 0x01,     // HID Version 1.11
    0x00,           // Country Code
    0x02            // Flags (Normally Connectable)
};

const uint8_t hid_report_map[] ={
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,       //   Usage Minimum (Left Control)
    0x29, 0xE7,       //   Usage Maximum (Right GUI)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8 modifier bits)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0x95, 0x01,       //   Report Count (reserved byte)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x03,       //   Input (Constant, Variable, Absolute)
    0x95, 0x05,       //   Report Count (five LED bits)
    0x75, 0x01,       //   Report Size (1)
    0x05, 0x08,       //   Usage Page (LEDs)
    0x19, 0x01,       //   Usage Minimum (Num Lock)
    0x29, 0x05,       //   Usage Maximum (Kana)
    0x91, 0x02,       //   Output (Data, Variable, Absolute)
    0x95, 0x01,       //   Report Count (LED padding)
    0x75, 0x03,       //   Report Size (3)
    0x91, 0x03,       //   Output (Constant, Variable, Absolute)
    0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
    0x95, 0x06,       //   Report Count (six simultaneous keys)
    0x75, 0x08,       //   Report Size (8)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x65,       //   Logical Maximum (Keyboard Application)
    0x19, 0x00,       //   Usage Minimum (No event)
    0x29, 0x65,       //   Usage Maximum (Keyboard Application)
    0x81, 0x00,       //   Input (Data, Array, Absolute)
    0xC0              // End Collection
};

const uint8_t hid_report_reference[] ={
    0x01,   // Report ID
    0x01    // Input Report
};

const uint8_t hid_output_report_reference[] ={
    0x01,   // Report ID
    0x02    // Output Report
};

const uint16_t hid_info_len = sizeof(hid_info);
const uint16_t hid_report_map_len = sizeof(hid_report_map);
const uint16_t hid_report_reference_len = sizeof(hid_report_reference);
const uint16_t hid_output_report_reference_len = sizeof(hid_output_report_reference);

// ---------------------Function Prototypes-----------------------

static uint8_t matrix_to_hid_usage(matrix_key_t key);
static void add_key(uint8_t usage);
static void remove_key(uint8_t usage);
static void print_report(void);

//---------------------------------------------------------------
//---------------------FUNCTION DEFINITIONS----------------------
//---------------------------------------------------------------

static uint8_t matrix_to_hid_usage(matrix_key_t key){
    switch (key){
        case KEY_UP:     return 0x52; // Keyboard Up Arrow
        case KEY_DOWN:   return 0x51; // Keyboard Down Arrow
        case KEY_LEFT:   return 0x50; // Keyboard Left Arrow
        case KEY_RIGHT:  return 0x4F; // Keyboard Right Arrow
        case KEY_CENTRE: return 0x2C; // Spacebar
        case KEY_A:      return 0x2A; // Delete
        case KEY_B:      return 0x28; // Enter
        case KEY_X:      return 0x29; // Escape
        case KEY_Y:      return 0x2B; // Tab
        default:          return 0;
    }
}

static void add_key(uint8_t usage){
    if (usage == 0) {
        return;
    }

    for (size_t i = 0; i < sizeof(report.keycodes); i++) {
        if (report.keycodes[i] == usage) {
            break;
        }

        if (report.keycodes[i] == 0) {
            report.keycodes[i] = usage;
            break;
        }
    }
}

static void remove_key(uint8_t usage){
    if (usage == 0) {
        return;
    }
    size_t write_idx = 0;
    
    for (size_t read_idx = 0; read_idx < sizeof(report.keycodes); read_idx++) {
        if (report.keycodes[read_idx] != usage && report.keycodes[read_idx] != 0) {
            report.keycodes[write_idx++] = report.keycodes[read_idx];
        }
    }
    while (write_idx < sizeof(report.keycodes)) {
        report.keycodes[write_idx++] = 0;
    }
}

static void print_report(void){
    printf("Keys: %02X %02X %02X %02X %02X %02X\n",
           report.keycodes[0], report.keycodes[1], report.keycodes[2],
           report.keycodes[3], report.keycodes[4], report.keycodes[5]);
}

//-----------PUBLIC FUNCTIONS--------------

void hid_core_init(void){
    memset(&report, 0, sizeof(report));

    printf("sizeof(hid_gamepad_report_t) = %u\n",
       (unsigned)sizeof(hid_gamepad_report_t));

}

void hid_core_press(matrix_key_t key){
    add_key(matrix_to_hid_usage(key));

    print_report();
}

void hid_core_release(matrix_key_t key){
    remove_key(matrix_to_hid_usage(key));

    print_report();
}

const hid_gamepad_report_t *hid_get_report(void){
    return &report;
}

uint8_t hid_get_protocol_mode(void){
    return hid_protocol_mode;
}

void hid_set_protocol_mode(uint8_t mode){
    hid_protocol_mode = mode;
}

uint8_t hid_get_control_point(void){
    return hid_control_point;
}

void hid_set_control_point(uint8_t value){
    hid_control_point = value;
}

uint8_t hid_get_led_state(void){
    return keyboard_led_state;
}

void hid_set_led_state(uint8_t state){
    keyboard_led_state = state;
}