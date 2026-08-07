#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Caminho de áudio nativo do ESP-IDF: driver I2S (driver/i2s_std.h) + ES8388
 * (es8388.c). Sem ESP-ADF — ver README para o racional (colisão de nomes de
 * arquivo do ESP-ADF com o esquema de build do PlatformIO). */

esp_err_t audio_codec_init(void);

/* Volume 0-VOLUME_STEPS (escala perceptual, ver config.h). Aplica curva
 * quadrática internamente antes de escrever no ES8388 (0-100) e persiste em
 * NVS (NVS_KEY_VOLUME_USER). */
esp_err_t audio_codec_set_volume(int volume);
int audio_codec_get_volume(void);

/* Como audio_codec_set_volume, mas NÃO persiste em NVS nem altera o valor
 * retornado por audio_codec_get_volume() — usado pelo AGC (audio_agc.c)
 * para modular o ganho de saída sem mexer no volume definido pelo usuário. */
esp_err_t audio_codec_apply_gain(int volume);

esp_err_t audio_codec_set_mute(bool mute);

/* Escreve PCM 16 bits estéreo já no sample rate configurado (ver
 * audio_codec_reconfigure_clock). Bloqueia até enviar tudo ou timeout. */
esp_err_t audio_codec_write(const uint8_t *data, size_t len, size_t *bytes_written);

/* A2DP pode informar sample rate diferente de faixa pra faixa (ex.: 44100 ou
 * 48000 Hz). Reconfigura o clock do I2S sem reinicializar o codec. */
esp_err_t audio_codec_reconfigure_clock(uint32_t sample_rate_hz);
