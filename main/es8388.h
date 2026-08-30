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

/* es8388_mic_init() dividido em duas metades, para poder configurar o codec
 * com o CLOCK I2S PARADO e so acordar o bloco digital depois que o clock
 * estiver correndo e estavel. Ver o comentario grande em
 * es8388_mic_config_begin() -- e a correcao da causa raiz do "ADC sobe
 * aleatoriamente travado/chiando".
 *
 * Uso correto (ver i2s_init em audio_codec.c):
 *   1. criar e inicializar os canais I2S, SEM habilitar
 *   2. es8388_mic_config_begin()
 *   3. habilitar RX e depois TX (o clock nasce aqui)
 *   4. es8388_mic_config_end()
 *
 * es8388_mic_init() continua existindo e faz as duas em sequencia, para
 * quem chamar com o clock ja correndo. */
esp_err_t es8388_mic_config_begin(void);
esp_err_t es8388_mic_config_end(void);

/* O PGA (ADCCONTROL1) NAO e exposto. Ele fica fixo em 0x77 (+21dB) dentro de
 * es8388_mic_config_begin(): 0x88 TRAVA o ADC desta placa entregando zeros, e
 * medido em 2026-08-30 o ganho do PGA nao altera em nada a relacao
 * sinal/ruido aqui -- o ruido que incomoda entra depois dele. Um controle a
 * mais so daria como estragar. Quem ajusta volume e o ganho digital, em
 * audio_codec_set_mic_gain(). */


/* Combinacoes de entrada analogica que fazem sentido nesta placa.
 *
 * O ESP32-A1S Audio Kit V2.2 herdou o roteamento de entrada da versao com
 * codec AC101 e NAO o adaptou ao ES8388, que tem menos entradas. O resultado,
 * confirmado no esquematico do modulo e no levantamento de Phil Schatzmann
 * ("The AI Thinker AudioKit Audio Input Bug", 2021-12-15):
 *
 *   MIC1P  (pino 17 do modulo) -> LIN1 do ES8388   microfone embutido ESQUERDO
 *   MIC2P  (pino 15)           -> RIN1             microfone embutido DIREITO
 *   MIC1N  (18) + LINEINL (22) -> LIN2             jack de entrada, ESQUERDO
 *   MIC2N  (14) + LINEINR (21) -> RIN2             jack de entrada, DIREITO
 *
 * Ou seja: os microfones embutidos estao em LIN1/RIN1 e a ENTRADA DE LINHA
 * (onde o receptor sem fio do usuario esta ligado) esta em LIN2/RIN2. Nao ha
 * como ler as duas ao mesmo tempo em canais separados.
 *
 * ES8388_IN_DIFF_MIC1 era o que este firmware usava. Ele le LIN1 menos RIN1,
 * isto e, a DIFERENCA ENTRE OS DOIS MICROFONES EMBUTIDOS -- que captam
 * praticamente o mesmo som, a poucos centimetros um do outro. A voz, que
 * chega igual nos dois, e quase toda cancelada na subtracao; o ruido proprio
 * de cada microfone, que e descorrelacionado, passa inteiro. E o pior arranjo
 * possivel, e explica os dois sintomas que sobreviveram a todos os ajustes de
 * ganho: voz baixa (obriga a cantar colado) e chiado alto. */
typedef enum {
    ES8388_IN_LIN1_SE = 0, /* 0x0A=0x00: LINPUT1/RINPUT1 -- microfones embutidos */
    ES8388_IN_LIN2_SE,     /* 0x0A=0x50: LINPUT2/RINPUT2 -- JACK DE ENTRADA */
    ES8388_IN_DIFF_MIC1,   /* 0x0A=0xF0 0x0B=0x02: diferencial LIN1-RIN1 */
    ES8388_IN_DIFF_MIC2,   /* 0x0A=0xF0 0x0B=0x82: diferencial LIN2-RIN2 */
    ES8388_IN_COUNT
} es8388_mic_input_t;

/* Seleciona a entrada e a memoriza -- es8388_mic_config_begin() reaplica o
 * ultimo valor escolhido, para que uma troca feita em runtime sobreviva a uma
 * reinicializacao do ADC. Escreve 0x0A e 0x0B juntos: eles so fazem sentido
 * em par (0x0B so e consultado quando 0x0A esta em modo diferencial), e
 * mexer num sem o outro ja produziu medicoes contraditorias aqui. */
esp_err_t es8388_mic_set_input_mode(es8388_mic_input_t modo);
es8388_mic_input_t es8388_mic_get_input_mode(void);
const char *es8388_mic_input_name(es8388_mic_input_t modo);

/* Leitura/escrita crua de registrador -- diagnostico ao vivo, ver
 * POST /api/mic/reg em web_server.c. */
esp_err_t es8388_write_reg_raw(uint8_t reg, uint8_t val);
esp_err_t es8388_read_reg_raw(uint8_t reg, uint8_t *out);
