#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ROWS 3
#define COLS 3

typedef enum{
    MATRIX_EVENT_NONE,
    MATRIX_EVENT_PRESS,
    MATRIX_EVENT_RELEASE
} matrix_event_type_t;

typedef enum{
    KEY_CENTRE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_A,
    KEY_B,
    KEY_X,
    KEY_Y,
} matrix_key_t;

typedef struct{
    matrix_event_type_t type;
    matrix_key_t key;
} matrix_event_t;

bool matrix_get_event(matrix_event_t *event);

void matrix_init(void);
void matrix_scan(void);
