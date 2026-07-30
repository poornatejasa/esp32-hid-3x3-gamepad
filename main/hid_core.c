#include "hid_core.h"

#include <stdio.h>
#include <string.h>

static hid_gamepad_report_t report;

static uint8_t matrix_to_hid_usage(matrix_key_t key);
static void add_key(uint8_t usage);
static void remove_key(uint8_t usage);
static void print_report(void);

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
