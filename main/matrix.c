#include "matrix.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_num.h"

#define EVENT_QUEUE_SIZE 16

typedef struct {
    matrix_event_t queue[EVENT_QUEUE_SIZE];
    uint8_t head;
    uint8_t tail;
} matrix_event_queue_t;

static const matrix_key_t keymap[ROWS][COLS] ={
    {KEY_X,    KEY_UP,  KEY_A},
    {KEY_LEFT, KEY_CENTRE,     KEY_RIGHT},
    {KEY_Y,     KEY_DOWN,     KEY_B}
};

static const gpio_num_t row_pins[ROWS] = {
    GPIO_NUM_0,
    GPIO_NUM_1,
    GPIO_NUM_8
};

static const gpio_num_t col_pins[COLS] = {
    GPIO_NUM_10,
    GPIO_NUM_11,
    GPIO_NUM_2
};

static bool key_state[ROWS][COLS];
static uint8_t debounce_count[ROWS][COLS];
static matrix_event_queue_t event_queue;


static void queue_push(matrix_event_t event);

void matrix_init(void){
    gpio_config_t io = {0};

    // Configure row pins as outputs
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = 0;

    for (int i = 0; i < ROWS; i++)
        io.pin_bit_mask |= (1ULL << row_pins[i]);

    gpio_config(&io);

    // Keep all rows HIGH initially
    for (int i = 0; i < ROWS; i++)
        gpio_set_level(row_pins[i], 1);

    // Configure column pins as inputs with pull-up
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pin_bit_mask = 0;

    for (int i = 0; i < COLS; i++)
        io.pin_bit_mask |= (1ULL << col_pins[i]);

    gpio_config(&io);

    event_queue.head = 0;
    event_queue.tail = 0;
}

void matrix_scan(void){
    for (int r = 0; r < ROWS; r++) {
        // Activate current row (active LOW)
        gpio_set_level(row_pins[r], 0);
        esp_rom_delay_us(5);

        for (int c = 0; c < COLS; c++) {
            bool raw_pressed = !gpio_get_level(col_pins[c]);

            if (raw_pressed != key_state[r][c]) {
                debounce_count[r][c]++;
                // Confirm state change after 2 consecutive matching scans (~20ms)
                if (debounce_count[r][c] >= 2) {
                    key_state[r][c] = raw_pressed;
                    debounce_count[r][c] = 0;

                    matrix_event_t ev = {
                        .type = raw_pressed ? MATRIX_EVENT_PRESS : MATRIX_EVENT_RELEASE,
                        .key = keymap[r][c]
                    };
                    queue_push(ev);
                }
            } else
                debounce_count[r][c] = 0;
        }

        // Deactivate current row
        gpio_set_level(row_pins[r], 1);
    }
}

bool matrix_get_event(matrix_event_t *event){
    if (event_queue.head == event_queue.tail) {
        event->type = MATRIX_EVENT_NONE;
        return false;
    }

    *event = event_queue.queue[event_queue.tail];
    event_queue.tail = (event_queue.tail + 1) % EVENT_QUEUE_SIZE;
    return true;
}

static void queue_push(matrix_event_t event) {
    uint8_t next = (event_queue.head + 1) % EVENT_QUEUE_SIZE;
    if (next != event_queue.tail) {
        event_queue.queue[event_queue.head] = event;
        event_queue.head = next;
    }
}