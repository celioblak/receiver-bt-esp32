#include "audio_agc.h"

#include <math.h>
#include <string.h>

#include "audio_codec.h"
#include "config.h"
#include "logger.h"
#include "storage.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_agc";

#define AGC_WINDOW_SAMPLES 1024 /* ~23ms a 44100Hz -- 256 (~6ms) reagia a cada
                                  * transiente/silabra isolada, causando o
                                  * ganho "bombear" (pumping) audivel */
#define AGC_TASK_PERIOD_MS 50 /* 20Hz */
#define AGC_GAIN_MIN 0.2f
#define AGC_GAIN_MAX 2.0f /* era 4.0 (+12dB) -- limita o pico de "estouro"
                            * quando o sinal cai muito por um instante */
/* Abaixo disso (silencio/quase-silencio -- ex.: notificacao do celular
 * tocando sozinha, ou o gap entre faixas) NAO ajusta o ganho: segurar em
 * vez de tentar "compensar" silencio evita o ganho cravar no maximo e
 * estourar quando o audio real volta. */
#define AGC_SILENCE_RMS_LINEAR 0.003f /* ~ -50dBFS */

typedef struct {
    float attack;
    float release;
} agc_coeffs_t;

/* Coeficientes reduzidos a ~40% dos originais: o AGC estava reagindo rapido
 * demais (constante de tempo de ~1s no modo "agressivo"), fazendo o volume
 * "subir e descer o tempo todo" de forma audivel. Nivelamento de loudness
 * de audio de verdade usa constantes de tempo de varios segundos, nao ~1s. */
static const agc_coeffs_t s_mode_coeffs[3] = {
    {0.002f, 0.0002f}, /* 0 = suave */
    {0.008f, 0.0008f}, /* 1 = medio */
    {0.02f,  0.004f},  /* 2 = agressivo */
};

/* Último bloco PCM entregue por bt_audio.c, protegido por s_window_mux
 * (produtor: task de I2S; consumidor: agc_task). */
static portMUX_TYPE s_window_mux = portMUX_INITIALIZER_UNLOCKED;
static int16_t s_window[AGC_WINDOW_SAMPLES];
static size_t s_window_len = 0;

/* current_gain é lido pela Web (api_status_get) e escrito pela agc_task —
 * tasks diferentes, protegido por critical section (ver regras do AGC). */
static portMUX_TYPE s_gain_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_current_gain = 1.0f;

static volatile bool s_enabled = false;
static volatile int8_t s_target_dbfs = DEFAULT_AGC_TARGET_DBFS;
static volatile uint8_t s_mode = DEFAULT_AGC_MODE;

static TaskHandle_t s_task_handle = NULL;

void audio_agc_feed(const int16_t *samples, size_t num_samples)
{
    if (!s_enabled) {
        return;
    }
    if (num_samples > AGC_WINDOW_SAMPLES) {
        samples += (num_samples - AGC_WINDOW_SAMPLES);
        num_samples = AGC_WINDOW_SAMPLES;
    }
    portENTER_CRITICAL(&s_window_mux);
    memcpy(s_window, samples, num_samples * sizeof(int16_t));
    s_window_len = num_samples;
    portEXIT_CRITICAL(&s_window_mux);
}

static void agc_task(void *arg)
{
    int16_t local[AGC_WINDOW_SAMPLES];

    for (;;) {
        if (!s_enabled) {
            vTaskSuspend(NULL);
            continue; /* retomado por audio_agc_enable(true) */
        }

        size_t len;
        portENTER_CRITICAL(&s_window_mux);
        len = s_window_len;
        memcpy(local, s_window, len * sizeof(int16_t));
        portEXIT_CRITICAL(&s_window_mux);

        if (len > 0) {
            float sum_sq = 0.0f;
            for (size_t i = 0; i < len; i++) {
                float s = local[i] / 32768.0f;
                sum_sq += s * s;
            }
            float rms = sqrtf(sum_sq / (float)len);

            if (rms >= AGC_SILENCE_RMS_LINEAR) {
                float target_linear = powf(10.0f, s_target_dbfs / 20.0f);
                float gain_needed = target_linear / rms;

                const agc_coeffs_t *coeffs = &s_mode_coeffs[s_mode];

                portENTER_CRITICAL(&s_gain_mux);
                float coeff = (gain_needed < s_current_gain) ? coeffs->attack : coeffs->release;
                s_current_gain += coeff * (gain_needed - s_current_gain);
                if (s_current_gain < AGC_GAIN_MIN) {
                    s_current_gain = AGC_GAIN_MIN;
                } else if (s_current_gain > AGC_GAIN_MAX) {
                    s_current_gain = AGC_GAIN_MAX;
                }
                portEXIT_CRITICAL(&s_gain_mux);
            }
            /* Abaixo do limiar de silencio: nao mexe em s_current_gain
             * (segura o ultimo ganho valido) -- ver AGC_SILENCE_RMS_LINEAR. */

            portENTER_CRITICAL(&s_gain_mux);
            float gain = s_current_gain;
            portEXIT_CRITICAL(&s_gain_mux);

            int user_vol = audio_codec_get_volume();
            float new_vol = user_vol * gain;
            if (new_vol < 0.0f) {
                new_vol = 0.0f;
            } else if (new_vol > VOLUME_STEPS) {
                new_vol = VOLUME_STEPS;
            }
            /* apply_gain, não set_volume: não deve persistir nem sobrescrever
             * o volume original do usuário. */
            audio_codec_apply_gain((int)new_vol);
        }

        vTaskDelay(pdMS_TO_TICKS(AGC_TASK_PERIOD_MS));
    }
}

void audio_agc_init(void)
{
    int32_t v;
    storage_get_i32(NVS_KEY_AGC_ENABLED, &v, DEFAULT_AGC_ENABLED);
    s_enabled = (v != 0);
    storage_get_i32(NVS_KEY_AGC_TARGET, &v, DEFAULT_AGC_TARGET_DBFS);
    s_target_dbfs = (int8_t)v;
    storage_get_i32(NVS_KEY_AGC_MODE, &v, DEFAULT_AGC_MODE);
    s_mode = (uint8_t)v;

    /* Prioridade baixa: não deve competir com a task de I2S (que roda em
     * configMAX_PRIORITIES - 3, ver bt_audio.c). Se desligada, a task se
     * auto-suspende no primeiro loop. Pilha 4096 (era 2048): o buffer
     * "local" sozinho já usa 2048 bytes (AGC_WINDOW_SAMPLES=1024 *
     * sizeof(int16_t)) — com 2048 de pilha estouraria. */
    xTaskCreate(agc_task, "agc_task", 4096, NULL, 3, &s_task_handle);

    logger_log(ESP_LOG_INFO, TAG, "AGC inicializado (enabled=%d target=%ddBFS mode=%d)",
               s_enabled, s_target_dbfs, s_mode);
}

void audio_agc_enable(bool enable)
{
    s_enabled = enable;
    storage_set_i32(NVS_KEY_AGC_ENABLED, enable ? 1 : 0);

    if (enable) {
        portENTER_CRITICAL(&s_gain_mux);
        s_current_gain = 1.0f;
        portEXIT_CRITICAL(&s_gain_mux);
        if (s_task_handle != NULL) {
            vTaskResume(s_task_handle);
        }
    } else {
        /* Restaura imediatamente o volume do usuário, sem esperar a task. */
        audio_codec_apply_gain(audio_codec_get_volume());
    }
}

void audio_agc_set_target(int8_t dbfs)
{
    if (dbfs < -30) {
        dbfs = -30;
    } else if (dbfs > -6) {
        dbfs = -6;
    }
    s_target_dbfs = dbfs;
    storage_set_i32(NVS_KEY_AGC_TARGET, dbfs);
}

void audio_agc_set_mode(uint8_t mode)
{
    if (mode > 2) {
        mode = 2;
    }
    s_mode = mode;
    storage_set_i32(NVS_KEY_AGC_MODE, mode);
}

bool audio_agc_is_enabled(void)
{
    return s_enabled;
}

int8_t audio_agc_get_target(void)
{
    return s_target_dbfs;
}

uint8_t audio_agc_get_mode(void)
{
    return s_mode;
}

float audio_agc_get_current_gain(void)
{
    portENTER_CRITICAL(&s_gain_mux);
    float gain = s_current_gain;
    portEXIT_CRITICAL(&s_gain_mux);
    return gain;
}
