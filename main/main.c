#include "config.h"
#include "logger.h"
#include "storage.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

void app_main(void)
{
    logger_init();
    ESP_ERROR_CHECK(storage_init());

    logger_log(ESP_LOG_INFO, TAG, "Receiver Bluetooth DIY - firmware %s", FW_VERSION);

    /* Round-trip de validação do wrapper de NVS: garante que o volume
     * persistido sobrevive a um reboot, usando o default na primeira vez. */
    int32_t volume = 0;
    storage_get_i32(NVS_KEY_VOLUME, &volume, DEFAULT_VOLUME);
    logger_log(ESP_LOG_INFO, TAG, "Volume carregado do NVS: %d", (int)volume);
    storage_set_i32(NVS_KEY_VOLUME, volume);

    while (1) {
        logger_log(ESP_LOG_INFO, TAG, "heap livre: %u bytes", (unsigned)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
