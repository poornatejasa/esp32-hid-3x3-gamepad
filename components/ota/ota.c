#include "ota.h"

#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_crc.h"

#include "esp_partition.h"
#include "esp_system.h"

//--------------------------Variables-----------------------------
static const char *TAG = "OTA";

typedef struct{
    ota_state_t state;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    size_t total_size;
    size_t bytes_received;
    uint32_t expected_crc32;
    uint32_t calculated_crc32;
} ota_context_t;

static ota_context_t ota_ctx;

//---------------------------------------------------------------
//---------------------FUNCTION DEFINITIONS----------------------
//---------------------------------------------------------------

//-----------Initialization-----------
void ota_init(void){
    ota_reset();
    ESP_LOGI(TAG, "OTA initialized");
}

void ota_reset(void){
    ota_ctx.state = OTA_STATE_IDLE;
    ota_ctx.handle = 0;
    ota_ctx.partition = NULL;
    ota_ctx.total_size = 0;
    ota_ctx.bytes_received = 0;
    ota_ctx.expected_crc32 = 0;
    ota_ctx.calculated_crc32 = 0;
}

//-------------Operations-------------
esp_err_t ota_begin(size_t image_size, uint32_t expected_crc32){
    //if (ota_ctx.state != OTA_STATE_IDLE)
    //    return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "ota_begin() called. State = %d", ota_ctx.state);
    if (ota_ctx.state != OTA_STATE_IDLE)
    {
        ESP_LOGE(TAG,
                "OTA already running. Current state = %d",
                ota_ctx.state);

        return ESP_ERR_INVALID_STATE;
    }

    ota_ctx.partition = esp_ota_get_next_update_partition(NULL);

    if (ota_ctx.partition == NULL){
        ESP_LOGE(TAG, "No OTA partition found");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "OTA partition: %s", ota_ctx.partition->label);

    esp_err_t err = esp_ota_begin(ota_ctx.partition, image_size, &ota_ctx.handle);
    if (err != ESP_OK){
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "OTA started (%u bytes)", (unsigned)image_size);

    ota_ctx.total_size = image_size;
    ota_ctx.bytes_received = 0;
    ota_ctx.expected_crc32 = expected_crc32;
    ota_ctx.calculated_crc32 = 0;
    ota_ctx.state = OTA_STATE_STARTED;

    return ESP_OK;
}

esp_err_t ota_write(const uint8_t *data, size_t length){
    if (ota_ctx.state != OTA_STATE_STARTED && ota_ctx.state != OTA_STATE_RECEIVING)
        return ESP_ERR_INVALID_STATE;

    if (data == NULL || length == 0)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = esp_ota_write(ota_ctx.handle, data, length);

    if (err != ESP_OK){
        ota_ctx.state = OTA_STATE_FAILED;
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return err;
    }

    ota_ctx.bytes_received += length;
    ota_ctx.calculated_crc32 = esp_crc32_le(ota_ctx.calculated_crc32,
                                            data, length);

    ota_ctx.state = OTA_STATE_RECEIVING;

    return ESP_OK;
}

esp_err_t ota_finish(void){
    if (ota_ctx.state != OTA_STATE_RECEIVING)
        return ESP_ERR_INVALID_STATE;

    if (ota_ctx.bytes_received != ota_ctx.total_size) {
        ESP_LOGE(TAG, "Image size mismatch: received=%u expected=%u",
                 (unsigned)ota_ctx.bytes_received, (unsigned)ota_ctx.total_size);
        esp_ota_abort(ota_ctx.handle);
        ota_ctx.handle = 0;
        ota_ctx.state = OTA_STATE_FAILED;
        return ESP_ERR_INVALID_SIZE;
    }

    if (ota_ctx.calculated_crc32 != ota_ctx.expected_crc32) {
        ESP_LOGE(TAG, "Image CRC mismatch: received=0x%08lX expected=0x%08lX",
                 (unsigned long)ota_ctx.calculated_crc32,
                 (unsigned long)ota_ctx.expected_crc32);
        esp_ota_abort(ota_ctx.handle);
        ota_ctx.handle = 0;
        ota_ctx.state = OTA_STATE_FAILED;
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "Verifying firmware...");

    ota_ctx.state = OTA_STATE_VERIFYING;
    esp_err_t err = esp_ota_end(ota_ctx.handle);
    ota_ctx.handle = 0;

    if (err != ESP_OK){
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        ota_ctx.state = OTA_STATE_FAILED;
        return err;
    }
    ESP_LOGI(TAG, "Firmware verified");

    ESP_LOGI(TAG, "Next boot partition: %s", ota_ctx.partition->label);
    err = esp_ota_set_boot_partition(ota_ctx.partition);

    if (err != ESP_OK){
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        ota_ctx.state = OTA_STATE_FAILED;
        return err;
    }
    ESP_LOGI(TAG, "OTA completed successfully");
    ota_ctx.state = OTA_STATE_COMPLETED;
    
    return ESP_OK;
}

void ota_reboot(void){
    if (ota_ctx.state != OTA_STATE_COMPLETED)
        return;
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void ota_abort(void){
    if (ota_ctx.handle != 0 && ota_ctx.state != OTA_STATE_COMPLETED){
        esp_ota_abort(ota_ctx.handle);
        ESP_LOGW(TAG, "OTA aborted");
    }
    ota_reset();
}

//---------------Status----------------
ota_state_t ota_get_state(void){
    return ota_ctx.state;
}

size_t ota_get_bytes_received(void){
    return ota_ctx.bytes_received;
}

size_t ota_get_total_size(void){
    return ota_ctx.total_size;
}

uint8_t ota_get_progress(void){
    if (ota_ctx.total_size == 0)
        return 0;

    return (uint8_t)
    (
        (ota_ctx.bytes_received * 100)
        / ota_ctx.total_size
    );
}
