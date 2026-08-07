#include "bt_audio.h"

#include <inttypes.h>
#include <string.h>

#include "audio_codec.h"
#include "config.h"
#include "logger.h"
#include "relay_control.h"
#include "storage.h"

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"

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

#define RINGBUF_HIGHEST_WATER_LEVEL  (32 * 1024)
#define RINGBUF_PREFETCH_WATER_LEVEL (20 * 1024)

typedef enum {
    RINGBUF_MODE_PROCESSING,
    RINGBUF_MODE_PREFETCHING,
    RINGBUF_MODE_DROPPING,
} ringbuf_mode_t;

static RingbufHandle_t s_ringbuf_i2s = NULL;
static SemaphoreHandle_t s_i2s_write_sem = NULL;
static TaskHandle_t s_i2s_task_handle = NULL;
static ringbuf_mode_t s_ringbuf_mode = RINGBUF_MODE_PROCESSING;

static size_t write_ringbuf(const uint8_t *data, size_t size)
{
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
                    s_ringbuf_mode = RINGBUF_MODE_PREFETCHING;
                    break;
                }
                size_t written = 0;
                audio_codec_write(data, item_size, &written);
                vRingbufferReturnItem(s_ringbuf_i2s, data);
            }
        }
    }
}

static void bt_i2s_task_start(void)
{
    s_ringbuf_mode = RINGBUF_MODE_PREFETCHING;
    s_i2s_write_sem = xSemaphoreCreateBinary();
    s_ringbuf_i2s = xRingbufferCreate(RINGBUF_HIGHEST_WATER_LEVEL, RINGBUF_TYPE_BYTEBUF);
    xTaskCreate(bt_i2s_task_handler, "bt_i2s_task", 2560, NULL, configMAX_PRIORITIES - 3, &s_i2s_task_handle);
}

static void bt_i2s_task_stop(void)
{
    if (s_i2s_task_handle) {
        vTaskDelete(s_i2s_task_handle);
        s_i2s_task_handle = NULL;
    }
    if (s_ringbuf_i2s) {
        vRingbufferDelete(s_ringbuf_i2s);
        s_ringbuf_i2s = NULL;
    }
    if (s_i2s_write_sem) {
        vSemaphoreDelete(s_i2s_write_sem);
        s_i2s_write_sem = NULL;
    }
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
            } else {
                logger_log(ESP_LOG_WARN, TAG, "Falha no pareamento, status=%d", param->auth_cmpl.stat);
            }
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            /* "Just Works": confirma automaticamente sem exigir interação do usuário. */
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
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
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                bt_i2s_task_stop();
                relay_control_notify_playing(false);
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            bool playing = (a2d->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
            logger_log(ESP_LOG_INFO, TAG, "A2DP audio %s", playing ? "iniciado" : "suspenso/parado");

            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.playing = playing;
            xSemaphoreGive(s_status_mutex);

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
            uint8_t oct0 = a2d->audio_cfg.mcc.cie.sbc[0];
            if (oct0 & (0x01 << 6)) {
                sample_rate = 32000;
            } else if (oct0 & (0x01 << 5)) {
                sample_rate = 44100;
            } else if (oct0 & (0x01 << 4)) {
                sample_rate = 48000;
            }
            if (oct0 & (0x01 << 3)) {
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

static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;

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
                request_track_metadata();
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
 * Inicialização
 * ------------------------------------------------------------------------- */

static void bt_stack_up(uint16_t event, void *p_param)
{
    char device_name[32];
    if (storage_get_str(NVS_KEY_DEVICE_NAME, device_name, sizeof(device_name)) != ESP_OK) {
        strlcpy(device_name, FW_DEVICE_NAME_DEFAULT, sizeof(device_name));
        storage_set_str(NVS_KEY_DEVICE_NAME, device_name);
    }
    esp_bt_gap_set_device_name(device_name);
    esp_bt_gap_register_callback(bt_app_gap_cb);

    /* Secure Simple Pairing "Just Works" — sem exigir confirmação manual do usuário. */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    ESP_ERROR_CHECK(esp_avrc_ct_init());
    esp_avrc_ct_register_callback(bt_app_rc_ct_cb);

    ESP_ERROR_CHECK(esp_a2d_sink_init());
    esp_a2d_register_callback(bt_app_a2d_cb);
    esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

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
    s_status_mutex = xSemaphoreCreateMutex();

    s_bt_app_task_queue = xQueueCreate(10, sizeof(bt_app_msg_t));
    xTaskCreate(bt_app_task_handler, "bt_app_task", 3072, NULL, 10, &s_bt_app_task_handle);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    bt_app_work_dispatch(bt_stack_up, 0, NULL, 0);
}
