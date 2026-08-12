#include "audio_codec.h"

#include <math.h>

#include "bt_audio.h"
#include "config.h"
#include "es8388.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "relay_control.h"
#include "storage.h"

static const char *TAG = "audio_codec";

static i2s_chan_handle_t s_tx_handle = NULL;
static int s_current_volume = DEFAULT_VOLUME_USER;
/* Nada protegia o canal I2S contra chamadas concorrentes de tasks
 * diferentes -- bt_audio (task dedicada), slimproto (escreve direto do seu
 * loop de rede) e o beep sob demanda (/api/system/beep, roda na task do
 * httpd) podiam todos cair em audio_codec_write()/reconfigure_clock() ao
 * mesmo tempo. reconfigure_clock() em especial desabilita o canal no meio
 * do caminho -- se isso acontecer enquanto outra task esta bloqueada dentro
 * de um i2s_channel_write() em andamento, e um cenario real de trava.
 * Confirmado na pratica: beep disparado durante playback do Slimproto
 * travou o dispositivo (precisou reset). */
static SemaphoreHandle_t s_i2s_mutex = NULL;
/* Espelha a taxa configurada em i2s_init(44100). Usado por
 * audio_codec_reconfigure_clock() pra pular o disable/reenable do canal
 * quando a taxa nao muda -- toda troca de faixa do Slimproto chama essa
 * funcao, mesmo quando o formato e identico ao da faixa anterior (comum,
 * ja que a biblioteca inteira do usuario e 44.1kHz/24-bit), e cada
 * disable/reenable desnecessario do canal I2S produz um "click" audivel
 * na troca -- o proprio engasgo residual reportado depois de ja corrigido
 * o vazamento de audio entre faixas. */
static uint32_t s_current_sample_rate_hz = 44100;

static esp_err_t i2s_init(uint32_t sample_rate_hz)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Sem novo audio a tempo, o driver por padrao REPETE o ultimo conteudo
     * transmitido em vez de silencio -- candidato direto pro ruido tipo
     * "tique" repetitivo que persiste em idle. auto_clear_after_cb faz o
     * driver zerar (silencio de verdade) automaticamente nesse caso. */
    chan_cfg.auto_clear_after_cb = true;
    /* Mais descritores de DMA = mais folga contra qualquer engasgo breve do
     * escalonador de coexistencia WiFi/BT (que pode preemptar outros
     * perifericos por instantes durante eventos de radio). */
    chan_cfg.dma_desc_num = 12;
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
    /* APLL foi tentado aqui (clock dedicado de audio) e revertido: com
     * MCLK no GPIO0 nesta placa, causou o I2S travar silenciosamente
     * (ring buffer enchia, nenhum audio saia) -- provavel problema de
     * roteamento do APLL nesse pino especifico via este driver. Mantendo
     * o clock padrao (PLL_F160M via I2S_CLK_SRC_DEFAULT). */

    err = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    return i2s_channel_enable(s_tx_handle);
}

esp_err_t audio_codec_init(void)
{
    s_i2s_mutex = xSemaphoreCreateMutex();

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

    int32_t saved_volume = DEFAULT_VOLUME_USER;
    storage_get_i32(NVS_KEY_VOLUME_USER, &saved_volume, DEFAULT_VOLUME_USER);
    audio_codec_set_volume((int)saved_volume);

    ESP_LOGI(TAG, "audio_codec pronto (I2S MCLK=%d BCLK=%d WS=%d DOUT=%d)",
             PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT);
    return ESP_OK;
}

/* Curva linear EM dB, não quadrática sobre a fração do slider: dB já É uma
 * escala perceptual (é assim que potenciômetros de áudio "de verdade" são
 * calibrados — tantos dB por grau de rotação), então aplicar x² por cima
 * dobra a compressão. Isso causava um bug real: es8388_set_volume(0-100)
 * atenua linearmente até -96dB, e x² jogava a metade do slider pra -72dB
 * (já inaudível na prática) — só os últimos ~20% do curso faziam
 * diferença perceptível. Restringimos a faixa útil a só os últimos
 * VOLUME_MAX_ATTEN_DB dB (abaixo disso já é imperceptível/mascarado por
 * ruído ambiente de qualquer forma; mudo de verdade é audio_codec_set_mute). */
#define VOLUME_MAX_ATTEN_DB 50.0f

static esp_err_t apply_curve_and_write(int volume_0_to_steps)
{
    if (volume_0_to_steps < 0) {
        volume_0_to_steps = 0;
    } else if (volume_0_to_steps > VOLUME_STEPS) {
        volume_0_to_steps = VOLUME_STEPS;
    }
    float normalized = (float)volume_0_to_steps / VOLUME_STEPS; /* 0.0-1.0 */
    /* es8388_set_volume(0-100) mapeia 0->0dB e 100->-96dB linearmente;
     * escolhemos es_vol pra restringir isso aos últimos VOLUME_MAX_ATTEN_DB
     * dB desse range em vez do range inteiro. */
    float es_vol_f = 100.0f - (VOLUME_MAX_ATTEN_DB / 0.96f) * (1.0f - normalized);
    uint8_t es_vol = (uint8_t)(es_vol_f < 0.0f ? 0.0f : es_vol_f);
    return es8388_set_volume(es_vol);
}

esp_err_t audio_codec_set_volume(int volume)
{
    esp_err_t err = apply_curve_and_write(volume);
    if (err == ESP_OK) {
        if (volume < 0) {
            volume = 0;
        } else if (volume > VOLUME_STEPS) {
            volume = VOLUME_STEPS;
        }
        s_current_volume = volume;
        storage_set_i32(NVS_KEY_VOLUME_USER, volume);
        bt_audio_notify_volume_changed(volume);
    }
    return err;
}

esp_err_t audio_codec_apply_gain(int volume)
{
    return apply_curve_and_write(volume);
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
    xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    esp_err_t err = i2s_channel_write(s_tx_handle, data, len, bytes_written, portMAX_DELAY);
    xSemaphoreGive(s_i2s_mutex);
    return err;
}

esp_err_t audio_codec_reconfigure_clock(uint32_t sample_rate_hz)
{
    /* i2s_channel_reconfig_std_clock() exige o canal desabilitado, senão
     * retorna ESP_ERR_INVALID_STATE e não muda nada — silenciosamente deixa
     * tocando na taxa antiga (distorcido/acelerado) para qualquer faixa que
     * não seja 44100Hz, o default usado em audio_codec_init(). Precisa do
     * mesmo mutex de audio_codec_write(): desabilitar o canal enquanto outra
     * task esta bloqueada dentro de um write() em andamento e a forma mais
     * direta de travar o dispositivo (confirmado na pratica). */
    xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    if (sample_rate_hz == s_current_sample_rate_hz) {
        /* Mesma taxa da faixa anterior (caso comum) -- pula o
         * disable/reenable, que produz um "click" audivel a toa. */
        xSemaphoreGive(s_i2s_mutex);
        return ESP_OK;
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    esp_err_t err = i2s_channel_disable(s_tx_handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_i2s_mutex);
        return err;
    }
    err = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
    if (err != ESP_OK) {
        i2s_channel_enable(s_tx_handle);
        xSemaphoreGive(s_i2s_mutex);
        return err;
    }
    err = i2s_channel_enable(s_tx_handle);
    if (err == ESP_OK) {
        s_current_sample_rate_hz = sample_rate_hz;
    }
    xSemaphoreGive(s_i2s_mutex);
    return err;
}

void audio_codec_play_test_tone(void)
{
    const uint32_t sample_rate = 44100;
    const float freq_hz = 440.0f;
    const float duration_s = 0.6f;
    const float amplitude = 0.8f; /* alto o suficiente pra ser audivel a distancia (ex.: pelo microfone de uma camera de monitoramento) */
    const uint32_t total_samples = (uint32_t)(sample_rate * duration_s);

    relay_control_notify_playing(true);
    /* Amplificadores externos costumam ter um "soft-start"/mudo de
     * protecao contra pop ao ligar (frequentemente 300ms-1s) -- com so
     * 50ms de folga, o bipe inteiro tocava e acabava ANTES do amplificador
     * terminar de "acordar", saindo em silencio mesmo com tudo certo do
     * nosso lado (codec, rele). */
    vTaskDelay(pdMS_TO_TICKS(800));
    audio_codec_set_mute(false);

    int16_t buf[256 * 2]; /* estereo */
    uint32_t done = 0;
    while (done < total_samples) {
        uint32_t chunk = total_samples - done;
        if (chunk > 256) {
            chunk = 256;
        }
        for (uint32_t i = 0; i < chunk; i++) {
            float t = (float)(done + i) / (float)sample_rate;
            int16_t sample = (int16_t)(amplitude * 32767.0f * sinf(2.0f * (float)M_PI * freq_hz * t));
            buf[i * 2] = sample;
            buf[i * 2 + 1] = sample;
        }
        size_t written = 0;
        audio_codec_write((const uint8_t *)buf, chunk * 2 * sizeof(int16_t), &written);
        done += chunk;
    }

    audio_codec_set_mute(true);
    relay_control_notify_playing(false); /* volta ao comportamento normal (desliga apos o timeout configurado) */
}
