#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AGC (Automatic Gain Control) opcional: normaliza o volume percebido entre
 * fontes diferentes (celular, TV, Echo Dot) ajustando o ganho de saída do
 * DAC via audio_codec_apply_gain(), sem alterar nem persistir o volume
 * definido pelo usuário (audio_codec_set_volume()/NVS_KEY_VOLUME_USER).
 *
 * Este projeto não usa ESP-ADF (ver README) — não existe um pipeline com
 * buffer de leitura para o AGC amostrar. Em vez disso, a task de I2S de
 * bt_audio.c entrega os blocos PCM que está prestes a tocar via
 * audio_agc_feed(), logo antes de audio_codec_write(). */

void  audio_agc_init(void);
void  audio_agc_enable(bool enable);
void  audio_agc_set_target(int8_t dbfs);   /* -30 a -6 dBFS */
void  audio_agc_set_mode(uint8_t mode);    /* 0=suave 1=medio 2=agressivo */
bool  audio_agc_is_enabled(void);
int8_t audio_agc_get_target(void);
uint8_t audio_agc_get_mode(void);
float audio_agc_get_current_gain(void);    /* p/ exibir na Web, ex.: 1.24 */

/* Chamado pela task de I2S (bt_audio.c) com o próximo bloco PCM 16 bits que
 * será escrito em audio_codec_write(). Não bloqueia. */
void audio_agc_feed(const int16_t *samples, size_t num_samples);
