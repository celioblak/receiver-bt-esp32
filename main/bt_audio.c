#include "bt_audio.h"

#include <inttypes.h>
#include <string.h>

#include "audio_agc.h"
#include "audio_codec.h"
#include "config.h"
#include "logger.h"
#include "pairing.h"
#include "relay_control.h"
#include "storage.h"

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bt_audio";

/* -------------------------------------------------------------------------
 * Estado atual (consultado pela API REST — ver web_server.c)
 * ------------------------------------------------------------------------- */

static SemaphoreHandle_t s_status_mutex = NULL;
static bt_audio_status_t s_status = {0};
static volatile bool s_discoverable = DEFAULT_BT_DISCOVERABLE;
/* 0 = nenhuma janela temporaria de descobrivel ativa. Ver bt_audio_enable_
 * discoverable_temporary()/bt_audio_check_discoverable_timeout(). */
static volatile int64_t s_discoverable_temp_deadline_us = 0;
/* Estado de visibilidade de ANTES da janela temporaria, pra restaurar quando
 * ela terminar. Sem isso a janela apagava em runtime uma visibilidade
 * PERMANENTE que estivesse ligada: ao expirar, s_discoverable ia pra false
 * mesmo com o NVS dizendo true -- o aparelho sumia da busca sem ninguem ter
 * pedido, e voltava sozinho no proximo reboot (inconsistencia flagrada em
 * auto-teste). */
static volatile bool s_discoverable_before_temp = false;
/* Timer de disparo unico que fecha a janela no instante exato. Antes isso
 * dependia so de bt_audio_check_discoverable_timeout() chamada pelo loop de
 * main.c, que roda a cada 30s DE PROPOSITO (cada publicacao MQTT e atividade
 * de radio, que acopla ruido audivel no ES8388 -- ver comentario la). Efeito
 * flagrado em auto-teste: uma janela de 10s continuava visivel bem depois do
 * prazo, e a contagem exibia "0s restantes" enquanto o aparelho ainda estava
 * pareavel. A checagem no loop continua existindo como rede de seguranca. */
static esp_timer_handle_t s_discoverable_timer = NULL;
static volatile bool s_require_pin = DEFAULT_BT_REQUIRE_PIN;

/* -------------------------------------------------------------------------
 * Fila de trabalho: os callbacks do Bluedroid rodam na task da pilha BT e
 * precisam retornar rápido. Todo processamento (log, AVRCP, etc.) é
 * despachado para bt_app_task_handler, seguindo o padrão do exemplo oficial
 * classic_bt/a2dp_sink do ESP-IDF.
 * ------------------------------------------------------------------------- */

typedef void (*bt_app_cb_t)(uint16_t event, void *param);

typedef struct {
    uint16_t event;
    bt_app_cb_t cb;
    void *param;
} bt_app_msg_t;

static QueueHandle_t s_bt_app_task_queue = NULL;
static TaskHandle_t s_bt_app_task_handle = NULL;

static bool bt_app_work_dispatch(bt_app_cb_t cb, uint16_t event, void *params, int param_len)
{
    bt_app_msg_t msg = {.event = event, .cb = cb, .param = NULL};

    if (param_len > 0 && params != NULL) {
        msg.param = malloc(param_len);
        if (msg.param == NULL) {
            return false;
        }
        memcpy(msg.param, params, param_len);
    }

    if (xQueueSend(s_bt_app_task_queue, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGE(TAG, "falha ao enfileirar trabalho (evento 0x%x)", event);
        free(msg.param);
        return false;
    }
    return true;
}

static void bt_app_task_handler(void *arg)
{
    bt_app_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_bt_app_task_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.cb) {
                msg.cb(msg.event, msg.param);
            }
            free(msg.param);
        }
    }
}

/* -------------------------------------------------------------------------
 * Ring buffer + task dedicada de I2S: desacopla o callback de dados do A2DP
 * (que não pode bloquear) da escrita bloqueante em audio_codec_write().
 * ------------------------------------------------------------------------- */

/* Descobrimos que a placa TEM PSRAM (8MB, esp_psram detecta e mapeia 4MB) —
 * o heap livre foi de ~11-26KB (brigando com WiFi+BT na DRAM interna) pra
 * ~4MB. Toda a novela de reduzir esse buffer pra 8-24KB pra não estourar o
 * heap (ver histórico no git) não é mais necessária; volta bem folgado —
 * 64KB absorve tranquilamente rajadas de pacotes A2DP sem picotar. */
#define RINGBUF_HIGHEST_WATER_LEVEL  (64 * 1024)
#define RINGBUF_PREFETCH_WATER_LEVEL (40 * 1024)

typedef enum {
    RINGBUF_MODE_PROCESSING,
    RINGBUF_MODE_PREFETCHING,
    RINGBUF_MODE_DROPPING,
} ringbuf_mode_t;

/* xRingbufferCreate() nao aceita capabilities -- o comentario antigo aqui
 * dizia "vem da PSRAM" mas isso nunca foi garantido pelo codigo, so
 * presumido (o alocador so cai pra PSRAM de qualquer jeito porque 64KB
 * jamais caberia nos ~178KB de RAM interna do chip inteiro, entao "deu
 * certo" por tamanho, nao por garantia). Trocado por xRingbufferCreateStatic
 * com o buffer pedido explicitamente em MALLOC_CAP_SPIRAM -- mesma ideia
 * de sempre: dados grandes em PSRAM de proposito, nao por acidente de
 * fallback do alocador. */
static uint8_t *s_ringbuf_i2s_storage = NULL;
static StaticRingbuffer_t s_ringbuf_i2s_struct;
static RingbufHandle_t s_ringbuf_i2s = NULL;
static SemaphoreHandle_t s_i2s_write_sem = NULL;
static TaskHandle_t s_i2s_task_handle = NULL;
static ringbuf_mode_t s_ringbuf_mode = RINGBUF_MODE_PROCESSING;

static size_t write_ringbuf(const uint8_t *data, size_t size)
{
    if (s_ringbuf_i2s == NULL) {
        /* bt_i2s_task_start() falhou ao alocar (sem heap contiguo
         * suficiente) — descarta em vez de crashar em xRingbufferSend. */
        return 0;
    }
    if (s_ringbuf_mode == RINGBUF_MODE_DROPPING) {
        size_t used = 0;
        vRingbufferGetInfo(s_ringbuf_i2s, NULL, NULL, NULL, NULL, &used);
        if (used <= RINGBUF_PREFETCH_WATER_LEVEL) {
            s_ringbuf_mode = RINGBUF_MODE_PROCESSING;
        }
        return 0;
    }

    BaseType_t done = xRingbufferSend(s_ringbuf_i2s, (void *)data, size, 0);
    if (!done) {
        ESP_LOGW(TAG, "ring buffer de audio cheio, descartando pacotes");
        s_ringbuf_mode = RINGBUF_MODE_DROPPING;
        return 0;
    }

    if (s_ringbuf_mode == RINGBUF_MODE_PREFETCHING) {
        size_t used = 0;
        vRingbufferGetInfo(s_ringbuf_i2s, NULL, NULL, NULL, NULL, &used);
        if (used >= RINGBUF_PREFETCH_WATER_LEVEL) {
            s_ringbuf_mode = RINGBUF_MODE_PROCESSING;
            xSemaphoreGive(s_i2s_write_sem);
        }
    }

    return size;
}

static void bt_i2s_task_handler(void *arg)
{
    const size_t max_chunk = 240 * 6;

    for (;;) {
        if (xSemaphoreTake(s_i2s_write_sem, portMAX_DELAY) == pdTRUE) {
            for (;;) {
                size_t item_size = 0;
                uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
                    s_ringbuf_i2s, &item_size, pdMS_TO_TICKS(20), max_chunk);
                if (item_size == 0) {
                    /* auto_clear_after_cb (ver audio_codec.c) ja zera o
                     * buffer DMA do I2S sozinho quando fica sem dado novo --
                     * nao precisa de flush manual aqui. */
                    s_ringbuf_mode = RINGBUF_MODE_PREFETCHING;
                    break;
                }
                audio_agc_feed((const int16_t *)data, item_size / sizeof(int16_t));
                size_t written = 0;
                audio_codec_write(data, item_size, &written);
                vRingbufferReturnItem(s_ringbuf_i2s, data);
            }
        }
    }
}

/* Aloca o ring buffer e o semaforo uma unica vez, no boot, antes do Wi-Fi
 * e do proprio controlador Bluetooth subirem (chamado no inicio de
 * bt_audio_init()). Alocar/liberar esses ~16KB a cada conexao A2DP fazia
 * a alocacao concorrer com WiFi/HTTP/mDNS ja fragmentando o heap havia
 * dezenas de segundos — o heap total livre parecia suficiente (~18KB),
 * mas o maior bloco contiguo as vezes nao chegava aos ~980 bytes que o
 * Bluedroid precisa pra remontar um pacote HCI, travando o dispositivo. */
static void bt_audio_prealloc_ring_buffer(void)
{
    s_i2s_write_sem = xSemaphoreCreateBinary();
    if (s_ringbuf_i2s_storage == NULL) {
        s_ringbuf_i2s_storage = heap_caps_malloc(RINGBUF_HIGHEST_WATER_LEVEL, MALLOC_CAP_SPIRAM);
    }
    if (s_ringbuf_i2s_storage != NULL) {
        s_ringbuf_i2s = xRingbufferCreateStatic(RINGBUF_HIGHEST_WATER_LEVEL, RINGBUF_TYPE_BYTEBUF,
                                                 s_ringbuf_i2s_storage, &s_ringbuf_i2s_struct);
    }
    if (s_i2s_write_sem == NULL || s_ringbuf_i2s == NULL) {
        ESP_LOGE(TAG, "falha ao pre-alocar buffer/semaforo de audio — sem audio em toda a sessao");
    }
}

static void bt_i2s_task_start(void)
{
    if (s_ringbuf_i2s == NULL || s_i2s_write_sem == NULL) {
        return; /* bt_audio_prealloc_ring_buffer() falhou no boot */
    }
    if (s_i2s_task_handle != NULL) {
        /* Ja tem uma rodando (ex.: evento de desconexao anterior nao
         * processado a tempo numa troca rapida de dispositivo) -- criar
         * outra faria duas tasks brigarem pelo mesmo ring buffer/semaforo.
         * bt_i2s_task_stop() sempre roda antes de uma nova conexao ser
         * aceita, entao isso so deveria disparar em cenario de corrida. */
        ESP_LOGW(TAG, "task de I2S ja estava rodando, nao criando outra");
        return;
    }
    s_ringbuf_mode = RINGBUF_MODE_PREFETCHING;
    if (xTaskCreate(bt_i2s_task_handler, "bt_i2s_task", 2560, NULL, configMAX_PRIORITIES - 3, &s_i2s_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "falha ao criar task de I2S (heap insuficiente) — sem audio nesta sessao");
    }
}

static void bt_i2s_task_stop(void)
{
    if (s_i2s_task_handle) {
        vTaskDelete(s_i2s_task_handle);
        s_i2s_task_handle = NULL;
    }
    /* Buffer e semaforo continuam vivos (ver bt_audio_prealloc_ring_buffer)
     * — só descarta qualquer resto de áudio da sessão anterior. */
    if (s_ringbuf_i2s) {
        size_t item_size;
        void *data;
        while ((data = xRingbufferReceive(s_ringbuf_i2s, &item_size, 0)) != NULL) {
            vRingbufferReturnItem(s_ringbuf_i2s, data);
        }
    }
    s_ringbuf_mode = RINGBUF_MODE_PROCESSING;
}

/* -------------------------------------------------------------------------
 * GAP — pareamento Secure Simple Pairing ("Just Works": aceita automático)
 * ------------------------------------------------------------------------- */

static void bt_app_gap_handler(uint16_t event, void *p_param)
{
    esp_bt_gap_cb_param_t *param = (esp_bt_gap_cb_param_t *)p_param;

    switch ((esp_bt_gap_cb_event_t)event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                logger_log(ESP_LOG_INFO, TAG, "Pareamento OK: %s", param->auth_cmpl.device_name);
                pairing_record_device(param->auth_cmpl.bda, (const char *)param->auth_cmpl.device_name);
            } else {
                logger_log(ESP_LOG_WARN, TAG, "Falha no pareamento, status=%d", param->auth_cmpl.stat);
            }
            /* limpa o codigo pendente (ver ESP_BT_GAP_KEY_NOTIF_EVT) --
             * pareamento terminou, com sucesso ou nao */
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.pending_pin_mac[0] = '\0';
            s_status.pending_pin_code[0] = '\0';
            xSemaphoreGive(s_status_mutex);
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            /* "Just Works": confirma automaticamente, exceto se houver lista de
             * dispositivos autorizados e este MAC não estiver nela. So
             * dispara com IO capability NoInputNoOutput (require_pin=false,
             * ver bt_stack_up) -- com require_pin=true o fluxo e Passkey
             * Entry (ESP_BT_GAP_KEY_NOTIF_EVT abaixo), sem confirmacao. */
            if (pairing_is_allowed(param->cfm_req.bda)) {
                esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            } else {
                logger_log(ESP_LOG_WARN, TAG, "Pareamento rejeitado (dispositivo nao autorizado)");
                esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, false);
            }
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT: {
            /* IO capability DisplayOnly (require_pin=true): o stack gerou
             * um passkey de 6 digitos que PRECISAMOS mostrar pro usuario
             * (sem tela fisica, mostramos via /api/status) -- a pessoa
             * digita esse numero no celular pra completar o pareamento.
             * A lista de autorizados (pairing_is_allowed) continua
             * valendo depois: ver o recheck em ESP_A2D_CONNECTION_STATE_EVT. */
            uint8_t *bda = param->key_notif.bda;
            logger_log(ESP_LOG_INFO, TAG, "Passkey pra parear [%02x:%02x:%02x:%02x:%02x:%02x]: %06" PRIu32,
                       bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], param->key_notif.passkey);
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            snprintf(s_status.pending_pin_mac, sizeof(s_status.pending_pin_mac),
                     "%02x:%02x:%02x:%02x:%02x:%02x", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            snprintf(s_status.pending_pin_code, sizeof(s_status.pending_pin_code),
                     "%06" PRIu32, param->key_notif.passkey);
            xSemaphoreGive(s_status_mutex);
            break;
        }
        case ESP_BT_GAP_KEY_REQ_EVT:
            break;
        default:
            break;
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    bt_app_work_dispatch((bt_app_cb_t)bt_app_gap_handler, event, param, sizeof(esp_bt_gap_cb_param_t));
}

/* -------------------------------------------------------------------------
 * A2DP
 * ------------------------------------------------------------------------- */

static const char *a2d_conn_state_str[] = {"desconectado", "conectando", "conectado", "desconectando"};

static void bt_app_a2d_handler(uint16_t event, void *p_param)
{
    esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)p_param;

    switch ((esp_a2d_cb_event_t)event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            uint8_t *bda = a2d->conn_stat.remote_bda;
            logger_log(ESP_LOG_INFO, TAG, "A2DP %s [%02x:%02x:%02x:%02x:%02x:%02x]",
                       a2d_conn_state_str[a2d->conn_stat.state],
                       bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

            bool connected = (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED);

            /* pairing_is_allowed() so e checado no pareamento inicial
             * (ESP_BT_GAP_CFM_REQ_EVT) -- um dispositivo que ja pareou
             * antes (com link key salva no controlador BT) reconecta
             * direto sem passar por ali de novo, ignorando a lista de
             * autorizados. Checar de novo aqui, na conexao, fecha essa
             * brecha: se nao for mais autorizado, desconecta e remove o
             * bond pra nao voltar a conectar sozinho. */
            if (connected && !pairing_is_allowed(bda)) {
                logger_log(ESP_LOG_WARN, TAG,
                           "Conexao rejeitada (dispositivo nao autorizado) [%02x:%02x:%02x:%02x:%02x:%02x]",
                           bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
                esp_bt_gap_remove_bond_device(bda);
                esp_a2d_sink_disconnect(bda);
                break;
            }

            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.connected = connected;
            if (connected) {
                snprintf(s_status.remote_mac, sizeof(s_status.remote_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            } else {
                s_status.playing = false;
                s_status.remote_mac[0] = '\0';
                s_status.title[0] = '\0';
                s_status.artist[0] = '\0';
                s_status.album[0] = '\0';
            }
            xSemaphoreGive(s_status_mutex);

            if (connected) {
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                bt_i2s_task_start();
            } else {
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                          s_discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
                bt_i2s_task_stop();
                relay_control_notify_playing(false);
                audio_codec_set_mute(true);
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            bool playing = (a2d->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
            logger_log(ESP_LOG_INFO, TAG, "A2DP audio %s", playing ? "iniciado" : "suspenso/parado");

            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.playing = playing;
            bool was_connected = s_status.connected;
            if (playing && !was_connected) {
                /* Rede de seguranca: se o audio esta tocando de verdade, o
                 * dispositivo esta obviamente conectado, mesmo que o
                 * ESP_A2D_CONNECTION_STATE_EVT correspondente tenha sido
                 * perdido/atrasado (fila cheia numa troca rapida de
                 * dispositivo, por exemplo) -- sem isso a interface web
                 * ficava mostrando "desconectado" com audio realmente
                 * tocando, e a task de I2S nunca chegava a iniciar. */
                uint8_t *bda = a2d->audio_stat.remote_bda;
                s_status.connected = true;
                snprintf(s_status.remote_mac, sizeof(s_status.remote_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            }
            xSemaphoreGive(s_status_mutex);

            if (playing && !was_connected) {
                logger_log(ESP_LOG_WARN, TAG, "Estado de conexao corrigido (evento de conexao perdido)");
                bt_i2s_task_start();
            }

            /* Mudo fora do estado "tocando": evita que ruido digital/RF
             * (WiFi, handshake do proprio Bluetooth) vaze pelo fone entre
             * faixas ou enquanto so esta conectado sem tocar nada. */
            audio_codec_set_mute(!playing);
            relay_control_notify_playing(playing);
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT: {
            if (a2d->audio_cfg.mcc.type != ESP_A2D_MCT_SBC) {
                ESP_LOGW(TAG, "codec A2DP nao suportado (tipo %d, so SBC)", a2d->audio_cfg.mcc.type);
                break;
            }
            int sample_rate = 16000;
            int channels = 2;
            const esp_a2d_cie_sbc_t *sbc = &a2d->audio_cfg.mcc.cie.sbc_info;
            if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
                sample_rate = 32000;
            } else if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
                sample_rate = 44100;
            } else if (sbc->samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
                sample_rate = 48000;
            }
            if (sbc->ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) {
                channels = 1;
            }
            audio_codec_reconfigure_clock((uint32_t)sample_rate);
            logger_log(ESP_LOG_INFO, TAG, "Audio configurado: %d Hz, %d canal(is)", sample_rate, channels);
            break;
        }
        default:
            break;
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bt_app_work_dispatch((bt_app_cb_t)bt_app_a2d_handler, event, param, sizeof(esp_a2d_cb_param_t));
}

static void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    write_ringbuf(data, len);
}

/* -------------------------------------------------------------------------
 * AVRCP (Controller) — metadados e mudança de faixa/estado de reprodução
 * ------------------------------------------------------------------------- */

#define RC_TL_GET_CAPS       0
#define RC_TL_GET_META_DATA  1
#define RC_TL_RN_TRACK       2
#define RC_TL_RN_PLAY_STATUS 3
#define RC_TL_PASSTHROUGH    4

static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;

/* Atraso antes de buscar metadados (titulo/artista/album) apos troca de
 * faixa: pedir na hora colocava trafego AVRCP (canal de controle)
 * disputando o mesmo radio BT com o inicio do audio A2DP da faixa nova,
 * causando engasgo audivel bem na troca (visto em log: rajada de
 * "Sequence numbers error" exatamente nesse momento, mesmo a <20cm do
 * celular -- nao e sinal fraco, e disputa interna WiFi/BT pelo radio
 * unico do ESP32). Dar essa folga deixa o audio da faixa nova estabilizar
 * primeiro. */
#define METADATA_FETCH_DELAY_US (1200 * 1000)
static esp_timer_handle_t s_metadata_delay_timer;

static void request_track_metadata(void)
{
    uint8_t attr_mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                         ESP_AVRC_MD_ATTR_ALBUM | ESP_AVRC_MD_ATTR_GENRE;
    esp_avrc_ct_send_metadata_cmd(RC_TL_GET_META_DATA, attr_mask);

    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                            ESP_AVRC_RN_TRACK_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(RC_TL_RN_TRACK, ESP_AVRC_RN_TRACK_CHANGE, 0);
    }
}

static void request_playback_status_notify(void)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                            ESP_AVRC_RN_PLAY_STATUS_CHANGE)) {
        esp_avrc_ct_send_register_notification_cmd(RC_TL_RN_PLAY_STATUS, ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    }
}

static void metadata_delay_timer_cb(void *arg)
{
    request_track_metadata();
}

static void bt_app_avrc_ct_handler(uint16_t event, void *p_param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)p_param;

    switch ((esp_avrc_ct_cb_event_t)event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            logger_log(ESP_LOG_INFO, TAG, "AVRCP %s", rc->conn_stat.connected ? "conectado" : "desconectado");
            if (rc->conn_stat.connected) {
                esp_avrc_ct_send_get_rn_capabilities_cmd(RC_TL_GET_CAPS);
            } else {
                s_avrc_peer_rn_cap.bits = 0;
            }
            break;

        case ESP_AVRC_CT_METADATA_RSP_EVT: {
            const char *attr_name = "?";
            char *dest = NULL;
            size_t dest_size = 0;

            switch (rc->meta_rsp.attr_id) {
                case ESP_AVRC_MD_ATTR_TITLE:
                    attr_name = "faixa";
                    dest = s_status.title;
                    dest_size = sizeof(s_status.title);
                    break;
                case ESP_AVRC_MD_ATTR_ARTIST:
                    attr_name = "artista";
                    dest = s_status.artist;
                    dest_size = sizeof(s_status.artist);
                    break;
                case ESP_AVRC_MD_ATTR_ALBUM:
                    attr_name = "album";
                    dest = s_status.album;
                    dest_size = sizeof(s_status.album);
                    break;
                case ESP_AVRC_MD_ATTR_GENRE:
                    attr_name = "genero";
                    break;
                default:
                    break;
            }

            logger_log(ESP_LOG_INFO, TAG, "%s: %.*s", attr_name, (int)rc->meta_rsp.attr_length, rc->meta_rsp.attr_text);

            if (dest != NULL) {
                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                size_t len = rc->meta_rsp.attr_length < dest_size - 1 ? rc->meta_rsp.attr_length : dest_size - 1;
                memcpy(dest, rc->meta_rsp.attr_text, len);
                dest[len] = '\0';
                xSemaphoreGive(s_status_mutex);
            }

            free(rc->meta_rsp.attr_text);
            break;
        }

        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            if (rc->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
                /* atrasado (METADATA_FETCH_DELAY_US) -- ver comentario
                 * acima de request_track_metadata() */
                esp_timer_stop(s_metadata_delay_timer); /* ESP_ERR_INVALID_STATE se ja parado, inofensivo */
                esp_timer_start_once(s_metadata_delay_timer, METADATA_FETCH_DELAY_US);
            } else if (rc->change_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
                logger_log(ESP_LOG_INFO, TAG, "Status de reproducao: 0x%x", rc->change_ntf.event_parameter.playback);
                request_playback_status_notify();
            }
            break;

        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
            s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;
            request_track_metadata();
            request_playback_status_notify();
            break;

        default:
            break;
    }
}

/* O callback do Bluedroid entrega meta_rsp.attr_text apontando para um
 * buffer estático interno da pilha — precisa ser copiado antes de
 * despachar para nossa fila (que já faz uma cópia rasa da struct, não do
 * ponteiro). Alocamos aqui, no contexto original do callback. */
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (event == ESP_AVRC_CT_METADATA_RSP_EVT) {
        uint8_t *attr_text = malloc(param->meta_rsp.attr_length + 1);
        if (attr_text == NULL) {
            return;
        }
        memcpy(attr_text, param->meta_rsp.attr_text, param->meta_rsp.attr_length);
        attr_text[param->meta_rsp.attr_length] = 0;
        param->meta_rsp.attr_text = attr_text;
    }

    bt_app_work_dispatch((bt_app_cb_t)bt_app_avrc_ct_handler, event, param, sizeof(esp_avrc_ct_cb_param_t));
}

/* -------------------------------------------------------------------------
 * AVRCP (Target) — volume absoluto: sem isso, o slider de volume do
 * celular fica sem efeito nenhum no som (o A2DP manda PCM em escala cheia
 * e quem decide o volume real é sempre o sink; o celular só sincroniza via
 * essa extensão do AVRCP, que precisa ser implementada nos dois lados).
 * ------------------------------------------------------------------------- */

/* true entre o REGISTER_NOTIFICATION do celular e nossa resposta CHANGED —
 * AVRCP só permite uma notificação não solicitada por registro; o celular
 * tem que re-registrar (o que ele faz automaticamente) para receber a próxima. */
static volatile bool s_avrc_vol_ntf_registered = false;

static void bt_app_avrc_tg_handler(uint16_t event, void *p_param)
{
    esp_avrc_tg_cb_param_t *rc = (esp_avrc_tg_cb_param_t *)p_param;

    switch ((esp_avrc_tg_cb_event_t)event) {
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
            /* celular -> receiver: volume vem em 0-127 (7 bits, padrão AVRCP) */
            int volume = (rc->set_abs_vol.volume * VOLUME_STEPS) / 127;
            audio_codec_set_volume(volume);
            break;
        }

        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
            if (rc->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                esp_avrc_rn_param_t param = {
                    .volume = (uint8_t)((audio_codec_get_volume() * 127) / VOLUME_STEPS),
                };
                esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &param);
                s_avrc_vol_ntf_registered = true;
            }
            break;

        default:
            break;
    }
}

static void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    bt_app_work_dispatch((bt_app_cb_t)bt_app_avrc_tg_handler, event, param, sizeof(esp_avrc_tg_cb_param_t));
}

void bt_audio_notify_volume_changed(int volume_0_to_steps)
{
    if (!s_avrc_vol_ntf_registered) {
        return;
    }
    s_avrc_vol_ntf_registered = false;

    esp_avrc_rn_param_t param = {
        .volume = (uint8_t)((volume_0_to_steps * 127) / VOLUME_STEPS),
    };
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &param);
}

void bt_audio_forget_device(const uint8_t mac[6])
{
    logger_log(ESP_LOG_INFO, TAG, "Esquecendo dispositivo [%02x:%02x:%02x:%02x:%02x:%02x]",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    pairing_set_allowed(mac, false);
    /* esp_a2d_sink_disconnect() em um MAC nao conectado so retorna erro,
     * inofensivo -- mais simples que checar s_status.remote_mac antes. */
    esp_a2d_sink_disconnect((uint8_t *)mac);
    esp_bt_gap_remove_bond_device((uint8_t *)mac);
    /* Acao deliberada do usuario -- nao faz sentido esperar o timeout
     * normal do rele (que existe pra nao "piscar" entre faixas). */
    relay_control_force_off();
}

void bt_audio_disconnect_device(const uint8_t mac[6])
{
    /* So derruba a conexao -- diferente de bt_audio_forget_device(), NAO
     * mexe no bond nem na lista de autorizados, entao o dispositivo pode
     * reconectar depois normalmente, sem precisar parear de novo. */
    logger_log(ESP_LOG_INFO, TAG, "Desconectando [%02x:%02x:%02x:%02x:%02x:%02x]",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_a2d_sink_disconnect((uint8_t *)mac);
    relay_control_force_off();
}

esp_err_t bt_audio_media_control(const char *cmd)
{
    uint8_t key_code;
    if (strcmp(cmd, "play") == 0) {
        key_code = ESP_AVRC_PT_CMD_PLAY;
    } else if (strcmp(cmd, "pause") == 0) {
        key_code = ESP_AVRC_PT_CMD_PAUSE;
    } else if (strcmp(cmd, "playpause") == 0) {
        /* AVRCP nao tem um "toggle" nativo -- decide com base no ultimo
         * estado de reproducao conhecido (ESP_A2D_AUDIO_STATE_EVT). */
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        bool playing = s_status.playing;
        xSemaphoreGive(s_status_mutex);
        key_code = playing ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY;
    } else if (strcmp(cmd, "stop") == 0) {
        key_code = ESP_AVRC_PT_CMD_STOP;
    } else if (strcmp(cmd, "next") == 0) {
        key_code = ESP_AVRC_PT_CMD_FORWARD;
    } else if (strcmp(cmd, "previous") == 0) {
        key_code = ESP_AVRC_PT_CMD_BACKWARD;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    /* Passthrough exige os dois eventos (tecla apertada e solta) pra
     * comandos momentaneos como play/pause/next/previous. */
    esp_avrc_ct_send_passthrough_cmd(RC_TL_PASSTHROUGH, key_code, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(RC_TL_PASSTHROUGH, key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
    return ESP_OK;
}

void bt_audio_set_discoverable(bool discoverable)
{
    /* Toggle manual/permanente cancela qualquer janela temporaria pendente --
     * inclusive desarmando o timer, senao ele dispararia depois e restauraria
     * o estado anterior por cima da escolha que acabou de ser feita aqui. */
    s_discoverable_temp_deadline_us = 0;
    if (s_discoverable_timer != NULL) {
        esp_timer_stop(s_discoverable_timer);
    }
    s_discoverable = discoverable;
    storage_set_i32(NVS_KEY_BT_DISCOVERABLE, discoverable ? 1 : 0);
    /* So aplica na hora se nao tiver ninguem conectado -- enquanto
     * conectado o modo ja fica NON_DISCOVERABLE (ver ESP_A2D_CONNECTION_
     * STATE_EVT), o que essa chamada respeitaria errado se disparasse aqui. */
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    bool connected = s_status.connected;
    xSemaphoreGive(s_status_mutex);
    if (!connected) {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                  discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
    }
}

bool bt_audio_get_discoverable(void)
{
    return s_discoverable;
}

/* Fecha a janela e devolve a visibilidade ao que era antes dela. Chamada pelo
 * timer (caminho normal), pela checagem do loop de main.c (rede de seguranca)
 * e pelo encerramento manual. */
static void discoverable_window_close(const char *motivo)
{
    if (s_discoverable_temp_deadline_us == 0) {
        return; /* nenhuma janela ativa */
    }
    s_discoverable_temp_deadline_us = 0;
    /* Restaura o que havia ANTES da janela (o NVS nunca foi escrito por ela).
     * Normalmente false; mas se a visibilidade permanente estava ligada, ela
     * continua ligada -- a janela nao deve revogar configuracao do usuario. */
    s_discoverable = s_discoverable_before_temp;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    bool connected = s_status.connected;
    xSemaphoreGive(s_status_mutex);
    if (!connected) {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                                  s_discoverable ? ESP_BT_GENERAL_DISCOVERABLE
                                                 : ESP_BT_NON_DISCOVERABLE);
    }
    logger_log(ESP_LOG_INFO, TAG, "bt_audio: janela de pareamento encerrada (%s, visivel=%d)",
               motivo, (int)s_discoverable);
}

static void discoverable_timer_cb(void *arg)
{
    (void)arg;
    discoverable_window_close("prazo");
}

void bt_audio_enable_discoverable_temporary(uint32_t duration_s)
{
    /* Guarda o estado anterior so na PRIMEIRA abertura -- chamar de novo pra
     * estender o prazo nao pode sobrescrever o valor salvo por "true". */
    if (s_discoverable_temp_deadline_us == 0) {
        s_discoverable_before_temp = s_discoverable;
    }
    s_discoverable_temp_deadline_us = esp_timer_get_time() + (int64_t)duration_s * 1000000LL;

    /* Timer de disparo unico pro instante exato do fim (ver comentario em
     * s_discoverable_timer). Recriar prazo cancela o anterior. */
    if (s_discoverable_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = discoverable_timer_cb,
            .name = "bt_disc_win",
        };
        if (esp_timer_create(&args, &s_discoverable_timer) != ESP_OK) {
            s_discoverable_timer = NULL; /* segue so com a checagem do loop */
        }
    }
    if (s_discoverable_timer != NULL) {
        esp_timer_stop(s_discoverable_timer); /* sem efeito se nao estiver armado */
        esp_timer_start_once(s_discoverable_timer, (uint64_t)duration_s * 1000000ULL);
    }
    /* Igual a bt_audio_set_discoverable(true), mas SEM persistir no NVS --
     * de proposito: se o dispositivo reiniciar no meio da janela, volta pro
     * padrao persistido (normalmente false), nao fica preso "descobrivel"
     * pra sempre por causa de uma janela temporaria que nunca expirou. */
    s_discoverable = true;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    bool connected = s_status.connected;
    xSemaphoreGive(s_status_mutex);
    if (!connected) {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    }
    logger_log(ESP_LOG_INFO, TAG, "bt_audio: descobrivel temporario ligado por %u s", (unsigned)duration_s);
}

void bt_audio_check_discoverable_timeout(void)
{
    /* Rede de seguranca: o caminho normal e o timer de disparo unico. Isto so
     * pega o caso de o timer nao ter sido criado (falta de recurso). */
    int64_t deadline = s_discoverable_temp_deadline_us;
    if (deadline == 0 || esp_timer_get_time() < deadline) {
        return;
    }
    discoverable_window_close("prazo (verificacao periodica)");
}

uint32_t bt_audio_get_discoverable_remaining_s(void)
{
    int64_t deadline = s_discoverable_temp_deadline_us;
    if (deadline == 0 || !s_discoverable) {
        return 0; /* visivel de forma permanente, ou desligado -- sem contagem */
    }
    int64_t restante = deadline - esp_timer_get_time();
    if (restante <= 0) {
        return 0; /* expirou; bt_audio_check_discoverable_timeout() ja vai desligar */
    }
    /* Arredonda pra cima: mostrar "0s restantes" enquanto ainda esta visivel
     * seria enganoso na interface. */
    return (uint32_t)((restante + 999999) / 1000000);
}

void bt_audio_stop_discoverable_temporary(void)
{
    if (s_discoverable_timer != NULL) {
        esp_timer_stop(s_discoverable_timer);
    }
    discoverable_window_close("manual");
}

void bt_audio_set_require_pin(bool require_pin)
{
    s_require_pin = require_pin;
    storage_set_i32(NVS_KEY_BT_REQUIRE_PIN, require_pin ? 1 : 0);
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = require_pin ? ESP_BT_IO_CAP_OUT : ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
}

bool bt_audio_get_require_pin(void)
{
    return s_require_pin;
}

/* -------------------------------------------------------------------------
 * Inicialização
 * ------------------------------------------------------------------------- */

static void bt_stack_up(uint16_t event, void *p_param)
{
    /* esp_coex_preference_set(ESP_COEX_PREFER_BT) foi tentado aqui pra
     * reduzir perda de pacote BT durante audio A2DP, mas piorou bastante
     * (rajada de "Sequence numbers error" foi de ~5s isolados pra ~8s
     * continuos) -- API deprecated, comportamento parece inconsistente
     * nesta versao do stack (5.5.3). Revertido; fica so o balanceamento
     * padrao do coexistidor. */

    char device_name[32];
    if (storage_get_str(NVS_KEY_DEVICE_NAME, device_name, sizeof(device_name)) != ESP_OK) {
        strlcpy(device_name, FW_DEVICE_NAME_DEFAULT, sizeof(device_name));
        storage_set_str(NVS_KEY_DEVICE_NAME, device_name);
    }
    esp_bt_gap_set_device_name(device_name);
    esp_bt_gap_register_callback(bt_app_gap_cb);

    int32_t v;
    storage_get_i32(NVS_KEY_BT_DISCOVERABLE, &v, DEFAULT_BT_DISCOVERABLE);
    s_discoverable = (v != 0);
    storage_get_i32(NVS_KEY_BT_REQUIRE_PIN, &v, DEFAULT_BT_REQUIRE_PIN);
    s_require_pin = (v != 0);

    /* Secure Simple Pairing: "Just Works" (NoInputNoOutput) por padrao --
     * sem confirmacao manual, so a lista de autorizados (pairing_is_allowed)
     * decide. Com require_pin=true, DisplayOnly forca o fluxo "Passkey
     * Entry": o stack gera um codigo de 6 digitos (ESP_BT_GAP_KEY_NOTIF_EVT)
     * que mostramos em /api/status, e a pessoa digita no celular -- funciona
     * de verdade em celulares modernos (diferente de PIN legado, que a
     * maioria ignora quando SSP esta disponivel). */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = s_require_pin ? ESP_BT_IO_CAP_OUT : ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    ESP_ERROR_CHECK(esp_avrc_ct_init());
    esp_avrc_ct_register_callback(bt_app_rc_ct_cb);

    /* Papel Target: só para volume absoluto (ver bt_app_avrc_tg_handler acima)
     * — sem isso o slider de volume do celular não tem nenhum efeito real. */
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    esp_avrc_tg_register_callback(bt_app_rc_tg_cb);
    esp_avrc_rn_evt_cap_mask_t tg_evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &tg_evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&tg_evt_set));

    ESP_ERROR_CHECK(esp_a2d_sink_init());
    esp_a2d_register_callback(bt_app_a2d_cb);
    esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE,
                              s_discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);

    logger_log(ESP_LOG_INFO, TAG, "Bluetooth pronto, nome: %s", device_name);
}

void bt_audio_get_status(bt_audio_status_t *out)
{
    if (out == NULL || s_status_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mutex);
}

void bt_audio_init(void)
{
    bt_audio_prealloc_ring_buffer();

    s_status_mutex = xSemaphoreCreateMutex();

    const esp_timer_create_args_t metadata_timer_args = {
        .callback = metadata_delay_timer_cb,
        .name = "avrc_meta_delay",
    };
    esp_timer_create(&metadata_timer_args, &s_metadata_delay_timer);

    /* 20 (era 10): rajadas de eventos numa troca rapida de dispositivo
     * (desconexao + pareamento + conexao + AVRCP + audio config, tudo em
     * poucos segundos) podem encher a fila; bt_app_work_dispatch() so
     * espera 10ms antes de descartar silenciosamente (so loga erro) se
     * a fila estiver cheia -- mais folga aqui custa bem pouco heap. */
    s_bt_app_task_queue = xQueueCreate(20, sizeof(bt_app_msg_t));
    xTaskCreate(bt_app_task_handler, "bt_app_task", 3072, NULL, 10, &s_bt_app_task_handle);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    /* Potencia de TX do BR/EDR no maximo (+9dBm, era +3dBm o padrao) --
     * tentativa de reduzir perda de pacote (BT_APPL: Sequence numbers
     * error) mesmo a curta distancia do celular. So ajuda de forma
     * indireta (ACKs/controle de fluxo mais fortes), a causa mais provavel
     * continua sendo disputa WiFi/BT pelo radio unico do ESP32 (ver
     * README). Precisa ser chamado aqui: depois do controller habilitado,
     * antes de qualquer transmissao (inquiry, pareamento, conexao). */
    esp_bredr_tx_power_set(ESP_PWR_LVL_N0, ESP_PWR_LVL_P9);

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    bt_app_work_dispatch(bt_stack_up, 0, NULL, 0);
}
