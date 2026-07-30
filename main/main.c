#include "matrix.h"
#include "hid_core.h"
#include "ble_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void initialization(void){
    matrix_init();
    hid_core_init();
    ble_transport_init();
}

void app_main(void){
    initialization();
    
    while (1){
        matrix_scan();
        matrix_event_t event;
        while (matrix_get_event(&event)){
            if (event.type == MATRIX_EVENT_PRESS){
                hid_core_press(event.key);
                //printf("Key pressed: %d\n", event.key);
            }
            else if (event.type == MATRIX_EVENT_RELEASE){
                hid_core_release(event.key);
                //printf("Key released: %d\n", event.key);
            }

            if (ble_transport_connected()){
                const hid_gamepad_report_t *report = hid_get_report();
                ble_transport_send_report((const uint8_t *)report, sizeof(*report));   
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
