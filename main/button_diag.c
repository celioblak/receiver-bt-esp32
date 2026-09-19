#include "button_diag.h"

#include <stdlib.h>

#include "audio_source.h"
#include "logger.h"
#include "wifi_manager.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button_diag";

/* KEY1 confirmado ao vivo (2026-08-22, testado isolado no log de
 * /api/logs): ADC1_CH0 / GPIO36, cai pra perto de 0 quando pressionado
 * (pull-up externo na placa, solto fica em ~4095).
 *
 * 2026-08-28: passou a ALTERNAR A FONTE DE AUDIO (Bluetooth <-> DLNA, ver
 * audio_source.h) -- pedido do usuario ao adotar o modelo de uma fonte ativa
 * por vez. Antes ligava/desligava o Wi-Fi, funcao que era so de diagnostico
 * do ruido de RF e continua disponivel via POST /api/wifi/disable_temp. */
#define KEY1_ADC_CHANNEL   ADC_CHANNEL_0
#define KEY1_PRESSED_BELOW 2000

/* Candidatos mais citados em referencias de terceiros (ex. squeezelite-esp32,
 * ESP-ADF) para os botoes KEY1-KEY6 do ESP32-A1S Audio Kit -- NAO
 * confirmados neste exemplar especifico ainda. Nenhum conflita com os pinos
 * ja usados neste firmware (I2C 32/33, I2S 0/25/26/27/35, rele 22, PA_ENABLE
 * 21 -- ver config.h). */
static const gpio_num_t s_candidate_gpios[] = {
    /* GPIO19 saiu da lista: confirmado como LED D5 desta placa (nao botao) e
     * agora usado como SAIDA por status_led.c -- monitora-lo como entrada
     * brigaria pelo pino. */
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_13, GPIO_NUM_18, GPIO_NUM_23,
};
#define NUM_CANDIDATES (sizeof(s_candidate_gpios) / sizeof(s_candidate_gpios[0]))

/* GPIO36 (ADC1_CH0) e GPIO39 (ADC1_CH3) sao entrada-somente, sem pull-up
 * interno -- exatamente o tipo de pino usado em esquemas de "escada de
 * resistores" com varios botoes num so ADC, outro padrao comum nessa placa. */
static const adc_channel_t s_adc_channels[] = {ADC_CHANNEL_0, ADC_CHANNEL_3}; /* GPIO36, GPIO39 */
#define NUM_ADC_CHANNELS (sizeof(s_adc_channels) / sizeof(s_adc_channels[0]))

static void button_diag_task(void *arg)
{
    (void)arg;

    for (size_t i = 0; i < NUM_CANDIDATES; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_candidate_gpios[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
    }

    adc_oneshot_unit_handle_t adc_handle = NULL;
    adc_oneshot_unit_init_cfg_t adc_init_cfg = {.unit_id = ADC_UNIT_1};
    bool adc_ok = (adc_oneshot_new_unit(&adc_init_cfg, &adc_handle) == ESP_OK);
    if (adc_ok) {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        for (size_t i = 0; i < NUM_ADC_CHANNELS; i++) {
            adc_oneshot_config_channel(adc_handle, s_adc_channels[i], &chan_cfg);
        }
    } else {
        logger_log(ESP_LOG_WARN, TAG, "falha ao iniciar ADC1 -- so os GPIOs digitais serao monitorados");
    }

    logger_log(ESP_LOG_INFO, TAG,
               "Diagnostico de botoes ativo -- aperte cada botao fisico da placa e observe "
               "esta tag no log (/api/logs)");

    int last_gpio_level[NUM_CANDIDATES];
    for (size_t i = 0; i < NUM_CANDIDATES; i++) {
        last_gpio_level[i] = gpio_get_level(s_candidate_gpios[i]);
    }
    int last_adc_value[NUM_ADC_CHANNELS] = {0};
    if (adc_ok) {
        for (size_t i = 0; i < NUM_ADC_CHANNELS; i++) {
            adc_oneshot_read(adc_handle, s_adc_channels[i], &last_adc_value[i]);
        }
    }
    /* Rearma so depois do botao ser solto -- sem isso, segurar o KEY1
     * apertado disparava a acao repetidamente a cada 100ms. */
    bool key1_armed = true;

    while (1) {
        for (size_t i = 0; i < NUM_CANDIDATES; i++) {
            int level = gpio_get_level(s_candidate_gpios[i]);
            if (level != last_gpio_level[i]) {
                logger_log(ESP_LOG_INFO, TAG, "GPIO%d mudou: %s", (int)s_candidate_gpios[i],
                           level == 0 ? "LOW (provavel botao pressionado)" : "HIGH (solto)");
                last_gpio_level[i] = level;
            }
        }
        if (adc_ok) {
            for (size_t i = 0; i < NUM_ADC_CHANNELS; i++) {
                int value = 0;
                if (adc_oneshot_read(adc_handle, s_adc_channels[i], &value) == ESP_OK) {
                    /* Limiar pra nao logar ruido normal do ADC (poucas
                     * unidades de flutuacao) -- so mudanca real de botao. */
                    if (abs(value - last_adc_value[i]) > 200) {
                        logger_log(ESP_LOG_INFO, TAG, "ADC1_CH%d (GPIO%s) mudou: %d -> %d",
                                   (int)s_adc_channels[i], s_adc_channels[i] == ADC_CHANNEL_0 ? "36" : "39",
                                   last_adc_value[i], value);
                        last_adc_value[i] = value;
                    }

                    if (s_adc_channels[i] == KEY1_ADC_CHANNEL) {
                        if (value < KEY1_PRESSED_BELOW && key1_armed) {
                            key1_armed = false;
                            logger_log(ESP_LOG_INFO, TAG, "KEY1 pressionado -- alternando fonte de audio");
                            audio_source_toggle();
                        } else if (value >= KEY1_PRESSED_BELOW) {
                            key1_armed = true;
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void button_diag_init(void)
{
    /* 2048: a task le GPIOs e um canal de ADC, sem nada de pilha profunda.
     * RAM interna e o recurso critico deste firmware. */
    xTaskCreate(button_diag_task, "button_diag", 2048, NULL, 3, NULL);
}
