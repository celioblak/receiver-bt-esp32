#pragma once

/* ESP32 Audio Kit V2.2 (ESP32-A1S) — codec ES8388.
 * Amplificador onboard (PA, GPIO21) não é usado neste projeto: o áudio sai
 * pela saída de linha para um amplificador externo. Ver board_pins_config.c
 * (get_pa_enable_gpio) e main/config.h (PIN_PA_ENABLE_DO_NOT_USE). */

#define FUNC_AUDIO_CODEC_EN (1)
#define BOARD_PA_GAIN       (0) /* não aplicável: PA onboard nunca é ativado */

extern audio_hal_func_t AUDIO_CODEC_ES8388_DEFAULT_HANDLE;

#define AUDIO_CODEC_DEFAULT_CONFIG(){                   \
        .adc_input  = AUDIO_HAL_ADC_INPUT_LINE1,        \
        .dac_output = AUDIO_HAL_DAC_OUTPUT_ALL,         \
        .codec_mode = AUDIO_HAL_CODEC_MODE_BOTH,        \
        .i2s_iface = {                                  \
            .mode = AUDIO_HAL_MODE_SLAVE,               \
            .fmt = AUDIO_HAL_I2S_NORMAL,                \
            .samples = AUDIO_HAL_48K_SAMPLES,           \
            .bits = AUDIO_HAL_BIT_LENGTH_16BITS,        \
        },                                              \
};
