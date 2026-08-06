#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Interface pública que o restante do ESP-ADF (es8388.c, i2s_stream.c,
 * esp_peripherals, ...) espera de um componente "audio_board". */

typedef struct {
    int mck_io_num;
    int bck_io_num;
    int ws_io_num;
    int data_out_num;
    int data_in_num;
} board_i2s_pin_t;

esp_err_t get_i2c_pins(i2c_port_t port, i2c_config_t *i2c_config);
esp_err_t get_i2s_pins(int port, board_i2s_pin_t *i2s_config);

int8_t get_sdcard_intr_gpio(void);
int8_t get_sdcard_open_file_num_max(void);
int8_t get_sdcard_power_ctrl_gpio(void);
int8_t get_auxin_detect_gpio(void);
int8_t get_headphone_detect_gpio(void);
int8_t get_pa_enable_gpio(void);
int8_t get_adc_detect_gpio(void);
int8_t get_es7243_mclk_gpio(void);
int8_t get_input_rec_id(void);
int8_t get_input_mode_id(void);
int8_t get_input_color_id(void);
int8_t get_input_set_id(void);
int8_t get_input_play_id(void);
int8_t get_input_volup_id(void);
int8_t get_input_voldown_id(void);
int8_t get_reset_codec_gpio(void);
int8_t get_reset_board_gpio(void);
int8_t get_green_led_gpio(void);
int8_t get_blue_led_gpio(void);

#ifdef __cplusplus
}
#endif
