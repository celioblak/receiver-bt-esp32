#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Driver mínimo do codec ES8388 (via I2C): DAC/line-out (usado desde o
 * início) e ADC/microfone (adicionado depois, ver es8388_mic_init).
 * Adaptado do driver MIT da Espressif (ESP-ADF audio_hal/driver/es8388),
 * removendo a camada audio_hal/board do ADF: usa diretamente os pinos de
 * main/config.h (PIN_I2C_SDA/PIN_I2C_SCL). */

esp_err_t es8388_init(void);
esp_err_t es8388_deinit(void);

/* Volume 0-100, mapeado para o range de atenuação digital do DAC (-96..0dB). */
esp_err_t es8388_set_volume(int volume);
esp_err_t es8388_get_volume(int *volume);

esp_err_t es8388_set_mute(bool mute);

/* Liga o bloco ADC do codec e seleciona a entrada MIC1 (par diferencial do
 * microfone onboard do ESP32 Audio Kit V2.2). Independente do DAC -- não
 * mexe em nada do caminho de reprodução (registrador ADCPOWER é separado de
 * DACPOWER). Ver comentário de DEFAULT_MIC_ENABLED em config.h: valores de
 * registrador ainda não validados em hardware real. */
esp_err_t es8388_mic_init(void);
esp_err_t es8388_mic_deinit(void);

/* Ganho do pré-amplificador (PGA) do microfone, 0-100 mapeado para os 8
 * passos de ~3dB (0 a +24dB) que o registrador do ES8388 suporta. */
esp_err_t es8388_mic_set_gain(int gain_0_to_100);

/* Escreve ADCCONTROL2 direto (selecao da entrada analogica do ADC) --
 * diagnostico pra descobrir em qual entrada o microfone desta placa esta
 * ligado, sem recompilar. Ver comentario na implementacao. */
/* Entrada do ADC onde o microfone desta placa realmente aparece (modo
 * diferencial) -- ver a medicao comentada em es8388.c. Exposto no header
 * porque audio_codec.c precisa REAPLICAR esse valor depois do boot (a
 * escrita feita durante a inicializacao nao pega). */
/* TEM QUE ACOMPANHAR ES8388_ADC_INPUT_MIC1 em es8388.c -- sao o MESMO valor,
 * duplicado porque audio_codec.c reaplica a selecao depois do boot. Ja
 * divergiram uma vez (2026-08-28): mudei so o .c pra 0xF0 e este ficou em
 * 0x50, entao mic_live_task desfazia a mudanca 1,5s apos cada boot e o modo
 * diferencial nunca valia no firmware gravado -- mas valia nos testes por
 * I2C ao vivo, o que fez os dois discordarem sem explicacao aparente. */
#define ES8388_ADC_INPUT_DEFAULT 0xF0

esp_err_t es8388_mic_set_input(uint8_t adccontrol2);

/* Reinicia so a maquina de estados do ADC (nao mexe no DAC/musica) --
 * recuperacao do caso "ADC saiu do boot entregando zeros". Ver comentario
 * na implementacao. */
esp_err_t es8388_mic_restart(void);

/* Leitura/escrita crua de registrador -- diagnostico ao vivo, ver
 * POST /api/mic/reg em web_server.c. */
esp_err_t es8388_write_reg_raw(uint8_t reg, uint8_t val);
esp_err_t es8388_read_reg_raw(uint8_t reg, uint8_t *out);
