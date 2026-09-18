#include "status_led.h"

#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "logger.h"
#include "wifi_manager.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "status_led";

/* Passo do laco. Todos os padroes sao multiplos disto. */
#define PASSO_MS 100

/* Padroes de piscada, um caractere por passo de 100ms: '1' aceso, '0' apagado.
 * A string inteira se repete. Escritos assim porque o padrao fica legivel de
 * bater o olho -- e o ponto do LED e justamente ser lido de bate-pronto. */
#define PAD_APAGADO      "0"
#define PAD_SEM_WIFI     "10"                 /* pisca rapido: 100ms on/off */
#define PAD_MIC_RUIM     "1010000000000"      /* duas piscadas curtas + pausa ~1s */
#define PAD_PAREAMENTO   "11111000000000000"  /* lento: 0,5s aceso, 1,2s apagado */

/* Nivel do GPIO que ACENDE o LED.
 *
 * ZERO: os LEDs desta placa sao ATIVOS EM NIVEL BAIXO -- confirmado ao vivo
 * (2026-08-30). Com LED_ACESO=1 o D5 ficava aceso justamente no estado
 * "apagado" (nivel 0), ou seja, o LED de diagnostico vivia ligado dizendo que
 * estava tudo bem. Se trocar por um LED externo com a outra polaridade, e so
 * voltar para 1. */
#define LED_ACESO 0

static const char *padrao_atual(void)
{
    /* Prioridade: o problema mais grave ganha o LED. Sem Wi-Fi o aparelho fica
     * inacessivel pela interface e pela API, entao nada mais importa -- e o
     * unico jeito de saber que o problema e ele, e nao a rede de quem procura. */
    if (!wifi_manager_is_connected() && !wifi_manager_radio_is_off()) {
        return PAD_SEM_WIFI;
    }
    /* Microfone habilitado mas com o conversor ruim: ele esta MUDO de
     * proposito, e ate agora so dava pra descobrir isso tentando cantar --
     * queixa direta do Celio ("fui cantar e o MIC nao havia subido"). O
     * firmware corrige sozinho cortando o MCLK; se este padrao persistir, e
     * porque as tentativas se esgotaram. */
    if (audio_codec_mic_is_enabled() &&
        strcmp(audio_codec_get_mic_adc_estado(), "saudavel") != 0) {
        return PAD_MIC_RUIM;
    }
    /* Janela de pareamento aberta: o aparelho esta visivel para qualquer um
     * por tempo limitado. Bom ter isso visivel de fora. */
    if (bt_audio_get_discoverable()) {
        return PAD_PAREAMENTO;
    }
    return PAD_APAGADO;
}

static void status_led_task(void *arg)
{
    (void)arg;
    const char *padrao = PAD_APAGADO;
    size_t pos = 0;

    for (;;) {
        const char *novo = padrao_atual();
        if (novo != padrao) {
            padrao = novo;
            pos = 0; /* recomeca o padrao do zero para ele ser reconhecivel */
        }
        bool aceso = (padrao[pos] == '1');
        gpio_set_level(PIN_STATUS_LED, aceso ? LED_ACESO : !LED_ACESO);
        pos++;
        if (padrao[pos] == '\0') {
            pos = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(PASSO_MS));
    }
}

void status_led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_STATUS_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_STATUS_LED, !LED_ACESO);

    if (xTaskCreate(status_led_task, "status_led", 2048, NULL, 2, NULL) != pdPASS) {
        logger_log(ESP_LOG_WARN, TAG, "nao foi possivel criar a task do LED de status");
        return;
    }
    logger_log(ESP_LOG_INFO, TAG, "LED de status (D5, GPIO%d) ativo", PIN_STATUS_LED);
}
