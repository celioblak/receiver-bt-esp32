#include "audio_codec.h"

#include "config.h"
#include "es8388.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "storage.h"

static const char *TAG = "audio_codec";

static i2s_chan_handle_t s_tx_handle = NULL;
static int s_current_volume = DEFAULT_VOLUME;

static esp_err_t i2s_init(uint32_t sample_rate_hz)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    return i2s_channel_enable(s_tx_handle);
}

esp_err_t audio_codec_init(void)
{
    esp_err_t err = es8388_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao inicializar o ES8388: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_init(44100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao inicializar o I2S: %s", esp_err_to_name(err));
        return err;
    }

    int32_t saved_volume = DEFAULT_VOLUME;
    storage_get_i32(NVS_KEY_VOLUME, &saved_volume, DEFAULT_VOLUME);
    audio_codec_set_volume((int)saved_volume);

    ESP_LOGI(TAG, "audio_codec pronto (I2S MCLK=%d BCLK=%d WS=%d DOUT=%d)",
             PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT);
    return ESP_OK;
}

esp_err_t audio_codec_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    esp_err_t err = es8388_set_volume(volume);
    if (err == ESP_OK) {
        s_current_volume = volume;
        storage_set_i32(NVS_KEY_VOLUME, volume);
    }
    return err;
}

int audio_codec_get_volume(void)
{
    return s_current_volume;
}

esp_err_t audio_codec_set_mute(bool mute)
{
    return es8388_set_mute(mute);
}

esp_err_t audio_codec_write(const uint8_t *data, size_t len, size_t *bytes_written)
{
    return i2s_channel_write(s_tx_handle, data, len, bytes_written, portMAX_DELAY);
}

esp_err_t audio_codec_reconfigure_clock(uint32_t sample_rate_hz)
{
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    return i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
}
