#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Driver mínimo do codec ES8388 (via I2C), só para reprodução (DAC/line-out).
 * Adaptado do driver MIT da Espressif (ESP-ADF audio_hal/driver/es8388),
 * removendo a camada audio_hal/board do ADF: usa diretamente os pinos de
 * main/config.h (PIN_I2C_SDA/PIN_I2C_SCL). ADC/microfone não é inicializado
 * nesta fase (fora do escopo do receiver — ver README para expansão futura
 * de karaokê). */

esp_err_t es8388_init(void);
esp_err_t es8388_deinit(void);

/* Volume 0-100, mapeado para o range de atenuação digital do DAC (-96..0dB). */
esp_err_t es8388_set_volume(int volume);
esp_err_t es8388_get_volume(int *volume);

esp_err_t es8388_set_mute(bool mute);
