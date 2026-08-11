#include "audio_agc.h"
#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "logger.h"
#include "mqtt_ha.h"
#include "pairing.h"
#include "relay_control.h"
#include "storage.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

void app_main(void)
{
    logger_init();
    ESP_ERROR_CHECK(storage_init());

    logger_log(ESP_LOG_INFO, TAG, "Receiver Bluetooth DIY - firmware %s", FW_VERSION);

    ESP_ERROR_CHECK(audio_codec_init());

    /* Muda o DAC já na inicialização: sem áudio real chegando ainda, ruído
     * digital/RF (WiFi, negociação Bluetooth) vazaria pelo fone o tempo
     * todo. Só desmuda quando o A2DP realmente começa a tocar (bt_audio.c). */
    audio_codec_set_mute(true);

    pairing_init();
    relay_control_init();
    bt_audio_init();
    audio_agc_init();
    wifi_manager_init();

    if (wifi_manager_network_available()) {
        web_server_start();
    }
    mqtt_ha_init();

    while (1) {
        logger_log(ESP_LOG_INFO, TAG, "heap livre: %u bytes", (unsigned)esp_get_free_heap_size());
        mqtt_ha_publish_state();
        /* 30s (nao 10s): cada publicacao MQTT e uma transmissao WiFi, e
         * atividade de radio (WiFi ou BT) acopla ruido audivel no estagio
         * analogico do ES8388 nesta placa (ver README). Trade-off: esta e a
         * unica fonte de atualizacao periodica do estado no Home Assistant
         * (nao ha publish orientado a evento em volume/playing ainda), entao
         * o estado la fica ate 30s desatualizado em vez de ate 10s. */
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
