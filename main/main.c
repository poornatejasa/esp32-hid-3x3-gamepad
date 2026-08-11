#include "matrix.h"

#include "hid_core.h"
#include "ble_hid.h"

#include "ota.h"

#include "ble_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"

static const char *TAG = "MAIN";

static void initialization(void){
    matrix_init();
    hid_core_init();
    ble_transport_init();
}

void app_main(void){
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s", running->label);

    ota_init();
    initialization();
    ESP_LOGI(TAG, "Normal Mode (OTA service available)");
    
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

            if (ble_hid_can_send_report()){
                const hid_gamepad_report_t *report = hid_get_report();
                ble_hid_send_report((const uint8_t *)report, sizeof(*report));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
