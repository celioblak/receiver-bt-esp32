#include "audio_codec.h"
#include "config.h"
#include "logger.h"
#include "storage.h"

#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define TEST_TONE_HZ 440
#define TEST_TONE_SAMPLE_RATE 44100
#define TEST_TONE_SECONDS 3

/* Gera um tom senoidal de teste e envia pelo I2S/ES8388, só para validar o
 * pipeline de áudio nesta etapa (ainda sem fonte Bluetooth — ver Etapa 4). */
static void play_test_tone(void)
{
    const int samples_per_chunk = 256;
    int16_t chunk[samples_per_chunk * 2]; /* estéreo */
    static uint32_t phase = 0;

    logger_log(ESP_LOG_INFO, TAG, "Tocando tom de teste (%d Hz, %d s)", TEST_TONE_HZ, TEST_TONE_SECONDS);

    int total_chunks = (TEST_TONE_SAMPLE_RATE * TEST_TONE_SECONDS) / samples_per_chunk;
    for (int c = 0; c < total_chunks; c++) {
        for (int i = 0; i < samples_per_chunk; i++) {
            float t = (float)phase / TEST_TONE_SAMPLE_RATE;
            int16_t sample = (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * TEST_TONE_HZ * t));
            chunk[i * 2] = sample;     /* esquerdo */
            chunk[i * 2 + 1] = sample; /* direito */
            phase++;
        }
        size_t written = 0;
        audio_codec_write((const uint8_t *)chunk, sizeof(chunk), &written);
    }

    logger_log(ESP_LOG_INFO, TAG, "Tom de teste concluído");
}

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

    ESP_ERROR_CHECK(audio_codec_init());
    play_test_tone();

    while (1) {
        logger_log(ESP_LOG_INFO, TAG, "heap livre: %u bytes", (unsigned)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
