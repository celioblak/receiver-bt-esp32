#include "board_pins_config.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "board_pins_config";

/* Pinagem do ESP32 Audio Kit V2.2 (ESP32-A1S). Estes valores devem ficar em
 * sincronia com PIN_I2C_* / PIN_I2S_* em main/config.h — duplicados aqui
 * (em vez de incluir main/config.h) para manter este componente
 * autocontido, já que "audio_board" é referenciado por nome por vários
 * componentes do ADF e não deve depender do componente "main". */
#define BOARD_PIN_I2C_SDA  33
#define BOARD_PIN_I2C_SCL  32
#define BOARD_PIN_I2S_MCLK 0
#define BOARD_PIN_I2S_BCLK 27
#define BOARD_PIN_I2S_WS   25
#define BOARD_PIN_I2S_DOUT 26
#define BOARD_PIN_I2S_DIN  35

esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config)
{
    if (i2c_config == NULL) {
        return ESP_FAIL;
    }
    if (port == I2C_NUM_0 || port == I2C_NUM_1) {
        i2c_config->sda_io_num = BOARD_PIN_I2C_SDA;
        i2c_config->scl_io_num = BOARD_PIN_I2C_SCL;
    } else {
        i2c_config->sda_io_num = -1;
        i2c_config->scl_io_num = -1;
        ESP_LOGE(TAG, "porta i2c %d não suportada", port);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config)
{
    if (i2s_config == NULL) {
        return ESP_FAIL;
    }
    if (port == 0 || port == 1) {
        i2s_config->mck_io_num = BOARD_PIN_I2S_MCLK;
        i2s_config->bck_io_num = BOARD_PIN_I2S_BCLK;
        i2s_config->ws_io_num = BOARD_PIN_I2S_WS;
        i2s_config->data_out_num = BOARD_PIN_I2S_DOUT;
        i2s_config->data_in_num = BOARD_PIN_I2S_DIN;
    } else {
        memset(i2s_config, -1, sizeof(board_i2s_pin_t));
        ESP_LOGE(TAG, "porta i2s %d não suportada", port);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* GPIO21 é o PA_ENABLE do amplificador onboard do kit. Este projeto envia
 * áudio para um amplificador externo e NUNCA deve ligar o amplificador
 * onboard. Retornar -1 faz o driver do ES8388 pular por completo a
 * configuração e o acionamento desse GPIO dentro de es8388_init()/
 * es8388_pa_power() — é intencional, não um pino "não encontrado". */
int8_t get_pa_enable_gpio(void)
{
    return -1;
}

/* Pinos abaixo não são usados nesta fase do projeto (SD card, botões, LEDs,
 * entrada auxiliar, fone de ouvido). Retornam -1 (não implementado) em vez
 * de um valor adivinhado, até serem validados fisicamente no hardware. */
int8_t get_sdcard_intr_gpio(void) { return -1; }
int8_t get_sdcard_open_file_num_max(void) { return 5; }
int8_t get_sdcard_power_ctrl_gpio(void) { return -1; }
int8_t get_auxin_detect_gpio(void) { return -1; }
int8_t get_headphone_detect_gpio(void) { return -1; }
int8_t get_adc_detect_gpio(void) { return -1; }
int8_t get_es7243_mclk_gpio(void) { return -1; }
int8_t get_input_rec_id(void) { return -1; }
int8_t get_input_mode_id(void) { return -1; }
int8_t get_input_color_id(void) { return -1; }
int8_t get_input_set_id(void) { return -1; }
int8_t get_input_play_id(void) { return -1; }
int8_t get_input_volup_id(void) { return -1; }
int8_t get_input_voldown_id(void) { return -1; }
int8_t get_reset_codec_gpio(void) { return -1; }
int8_t get_reset_board_gpio(void) { return -1; }
int8_t get_green_led_gpio(void) { return -1; }
int8_t get_blue_led_gpio(void) { return -1; }
