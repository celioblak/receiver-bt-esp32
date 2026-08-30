#include "audio_source.h"

#include "audio_codec.h"
#include "bt_audio.h"
#include "dlna_renderer.h"
#include "logger.h"
#include "relay_control.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "audio_source";

/* volatile: lido pelas tasks de I2S/busca das duas fontes (prioridades
 * diferentes) a cada bloco de audio, escrito pela task do botao/httpd. */
static volatile audio_source_t s_source = AUDIO_SOURCE_BT;
/* Serializa trocas concorrentes (botao fisico + API web ao mesmo tempo) --
 * sem isso duas trocas simultaneas poderiam intercalar os avisos aos
 * subsistemas e deixar os dois ativos (ou nenhum). */
static SemaphoreHandle_t s_switch_mutex = NULL;

void audio_source_init(void)
{
    s_switch_mutex = xSemaphoreCreateMutex();
    s_source = AUDIO_SOURCE_BT;
    logger_log(ESP_LOG_INFO, TAG, "Fonte de audio inicial: %s", audio_source_name(s_source));
}

audio_source_t audio_source_get(void)
{
    return s_source;
}

bool audio_source_is_bt(void)
{
    return s_source == AUDIO_SOURCE_BT;
}

bool audio_source_is_dlna(void)
{
    return s_source == AUDIO_SOURCE_DLNA;
}

const char *audio_source_name(audio_source_t src)
{
    return src == AUDIO_SOURCE_DLNA ? "dlna" : "bluetooth";
}

void audio_source_set(audio_source_t src)
{
    if (s_switch_mutex != NULL) {
        xSemaphoreTake(s_switch_mutex, portMAX_DELAY);
    }

    if (src == s_source) {
        if (s_switch_mutex != NULL) {
            xSemaphoreGive(s_switch_mutex);
        }
        return;
    }

    audio_source_t anterior = s_source;
    logger_log(ESP_LOG_WARN, TAG, "Trocando fonte de audio: %s -> %s",
               audio_source_name(anterior), audio_source_name(src));

    /* Ordem importa: silencia ANTES de trocar (evita estalo/residuo da fonte
     * antiga sair no meio da transicao), depois desativa a antiga, so entao
     * marca a nova como ativa. Assim nunca existe um instante com as duas se
     * achando ativas. */
    audio_codec_set_mute(true);
    relay_control_notify_playing(false);

    if (anterior == AUDIO_SOURCE_BT) {
        bt_audio_on_source_deactivated();
    } else {
        dlna_renderer_on_source_deactivated();
    }

    s_source = src;

    if (src == AUDIO_SOURCE_BT) {
        bt_audio_on_source_activated();
    } else {
        dlna_renderer_on_source_activated();
    }

    logger_log(ESP_LOG_INFO, TAG, "Fonte de audio agora: %s", audio_source_name(s_source));

    if (s_switch_mutex != NULL) {
        xSemaphoreGive(s_switch_mutex);
    }
}

void audio_source_toggle(void)
{
    audio_source_set(audio_source_is_bt() ? AUDIO_SOURCE_DLNA : AUDIO_SOURCE_BT);
}
