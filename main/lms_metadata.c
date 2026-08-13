#include "lms_metadata.h"

#include "cJSON.h"
#include "config.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "logger.h"
#include "storage.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "lms_metadata";

/* Payloads reais observados (testado ao vivo contra o MA) chegam a ~6-8KB
 * (metadados ricos: bio do artista, varias imagens etc, a maior parte
 * irrelevante pra nos). O componente entrega em fragmentos via eventos
 * WEBSOCKET_EVENT_DATA -- juntamos aqui num buffer generoso (PSRAM
 * sobrando) em vez de tentar filtrar campos no protocolo (a API do MA nao
 * suporta selecao de campos como o "tags:" do LMS classico). */
#define LMS_MSG_BUF_MAX (24 * 1024)
#define LMS_POLL_INTERVAL_US (3 * 1000 * 1000)

static esp_websocket_client_handle_t s_client = NULL;
static esp_timer_handle_t s_poll_timer = NULL;
static char s_queue_id[24] = {0}; /* "up" + mac sem ':', ex.: up704bca253ba8 */

static char *s_msg_buf = NULL;
static size_t s_msg_len = 0;
static bool s_msg_overflow = false;

static SemaphoreHandle_t s_mutex = NULL;
static lms_metadata_t s_metadata = {0};
static volatile bool s_authenticated = false;
static volatile bool s_configured = false;
static volatile bool s_auth_failed = false;

static void derive_queue_id(void)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_queue_id, sizeof(s_queue_id), "up%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void send_auth(void)
{
    char token[400];
    if (storage_get_str(NVS_KEY_MA_TOKEN, token, sizeof(token)) != ESP_OK || token[0] == '\0') {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "message_id", "auth");
    cJSON_AddStringToObject(root, "command", "auth");
    cJSON *args = cJSON_AddObjectToObject(root, "args");
    cJSON_AddStringToObject(args, "token", token);
    char *text = cJSON_PrintUnformatted(root);
    if (text != NULL) {
        esp_websocket_client_send_text(s_client, text, (int)strlen(text), pdMS_TO_TICKS(2000));
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

static void send_now_playing_query(void)
{
    if (s_client == NULL || !s_authenticated || !esp_websocket_client_is_connected(s_client)) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "message_id", "np");
    cJSON_AddStringToObject(root, "command", "player_queues/get");
    cJSON *args = cJSON_AddObjectToObject(root, "args");
    cJSON_AddStringToObject(args, "queue_id", s_queue_id);
    char *text = cJSON_PrintUnformatted(root);
    if (text != NULL) {
        esp_websocket_client_send_text(s_client, text, (int)strlen(text), pdMS_TO_TICKS(2000));
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

static void poll_timer_cb(void *arg)
{
    send_now_playing_query();
}

/* Extrai um campo string de "obj[field]" (ou NULL/objeto ausente) pra "out",
 * truncando em out_len -- todos os campos de texto reais (titulo, nome de
 * artista, album) sao curtos o bastante pra nao perder informacao util. */
static void copy_str_field(cJSON *obj, const char *field, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(obj, field);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(out, item->valuestring, out_len);
    } else {
        out[0] = '\0';
    }
}

static void handle_auth_response(cJSON *result)
{
    cJSON *authenticated = cJSON_GetObjectItem(result, "authenticated");
    if (cJSON_IsTrue(authenticated)) {
        s_authenticated = true;
        s_auth_failed = false;
        logger_log(ESP_LOG_INFO, TAG, "lms_metadata: autenticado na API do Music Assistant");
        send_now_playing_query();
    } else {
        s_authenticated = false;
        s_auth_failed = true;
        logger_log(ESP_LOG_WARN, TAG, "lms_metadata: falha na autenticacao (token invalido/expirado?)");
    }
}

static void handle_now_playing_response(cJSON *result)
{
    lms_metadata_t updated = {0};

    cJSON *state = cJSON_GetObjectItem(result, "state");
    updated.playing = cJSON_IsString(state) && strcmp(state->valuestring, "playing") == 0;

    cJSON *current_item = cJSON_GetObjectItem(result, "current_item");
    if (cJSON_IsObject(current_item)) {
        cJSON *media_item = cJSON_GetObjectItem(current_item, "media_item");
        if (cJSON_IsObject(media_item)) {
            copy_str_field(media_item, "name", updated.title, sizeof(updated.title));

            cJSON *artists = cJSON_GetObjectItem(media_item, "artists");
            if (cJSON_IsArray(artists) && cJSON_GetArraySize(artists) > 0) {
                copy_str_field(cJSON_GetArrayItem(artists, 0), "name", updated.artist, sizeof(updated.artist));
            }

            cJSON *album = cJSON_GetObjectItem(media_item, "album");
            if (cJSON_IsObject(album)) {
                copy_str_field(album, "name", updated.album, sizeof(updated.album));
            }
        } else {
            /* Sem media_item (ex.: radio ao vivo) -- current_item.name ja
             * costuma vir como "Artista - Titulo" combinado; usamos como
             * titulo mesmo assim, melhor que nada. */
            copy_str_field(current_item, "name", updated.title, sizeof(updated.title));
        }
    }

    updated.valid = true;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_metadata = updated;
    xSemaphoreGive(s_mutex);
}

static void handle_message(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return;
    }

    cJSON *message_id = cJSON_GetObjectItem(root, "message_id");
    if (cJSON_IsString(message_id)) {
        cJSON *result = cJSON_GetObjectItem(root, "result");
        bool is_auth = strcmp(message_id->valuestring, "auth") == 0;
        if (is_auth && cJSON_IsObject(result)) {
            handle_auth_response(result);
        } else if (is_auth) {
            /* Resposta de erro (ex.: {"message_id":"auth","error_code":...,
             * "details":"..."}) -- sem "result", mas ainda e uma falha de
             * autenticacao de verdade (token malformado, por exemplo). */
            s_authenticated = false;
            s_auth_failed = true;
            cJSON *details = cJSON_GetObjectItem(root, "details");
            logger_log(ESP_LOG_WARN, TAG, "lms_metadata: erro na autenticacao: %s",
                       cJSON_IsString(details) ? details->valuestring : "?");
        } else if (strcmp(message_id->valuestring, "np") == 0 && cJSON_IsObject(result)) {
            handle_now_playing_response(result);
        }
        /* outras mensagens (eventos nao usados) sao ignoradas de proposito */
    }

    cJSON_Delete(root);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            logger_log(ESP_LOG_INFO, TAG, "lms_metadata: conectado na API do Music Assistant");
            s_authenticated = false;
            s_msg_len = 0;
            s_msg_overflow = false;
            send_auth();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            s_authenticated = false;
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code != 0x01 /* WS_TRANSPORT_OPCODES_TEXT */) {
                break;
            }
            /* payload_offset==0 marca o inicio de uma mensagem nova --
             * zera o acumulador (protege contra uma mensagem anterior
             * incompleta/nunca fechada por algum motivo). */
            if (data->payload_offset == 0) {
                s_msg_len = 0;
                s_msg_overflow = false;
            }
            if (!s_msg_overflow && s_msg_buf != NULL) {
                if (s_msg_len + data->data_len <= LMS_MSG_BUF_MAX) {
                    memcpy(s_msg_buf + s_msg_len, data->data_ptr, data->data_len);
                    s_msg_len += data->data_len;
                } else {
                    s_msg_overflow = true; /* mensagem maior que esperavamos -- descarta */
                }
            }
            if (!s_msg_overflow && (size_t)(data->payload_offset + data->data_len) >= (size_t)data->payload_len) {
                handle_message(s_msg_buf, s_msg_len);
                s_msg_len = 0;
            }
            break;

        default:
            break;
    }
}

void lms_metadata_init(void)
{
    char token[400];
    if (storage_get_str(NVS_KEY_MA_TOKEN, token, sizeof(token)) != ESP_OK || token[0] == '\0') {
        return; /* recurso desativado em silencio, mesmo padrao do MQTT sem broker */
    }

    char host[64];
    if (storage_get_str(NVS_KEY_SLIM_HOST, host, sizeof(host)) != ESP_OK || host[0] == '\0') {
        logger_log(ESP_LOG_WARN, TAG, "lms_metadata: token configurado mas slim_host vazio -- desativado");
        return;
    }

    s_msg_buf = malloc(LMS_MSG_BUF_MAX);
    if (s_msg_buf == NULL) {
        logger_log(ESP_LOG_ERROR, TAG, "lms_metadata: sem heap para buffer de mensagens");
        return;
    }

    s_mutex = xSemaphoreCreateMutex();
    derive_queue_id();

    char uri[96];
    snprintf(uri, sizeof(uri), "ws://%s:8095/ws", host);

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 5000,
    };
    s_client = esp_websocket_client_init(&ws_cfg);
    if (s_client == NULL) {
        logger_log(ESP_LOG_ERROR, TAG, "lms_metadata: falha ao criar cliente websocket");
        return;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_client);
    s_configured = true;

    const esp_timer_create_args_t timer_args = {
        .callback = poll_timer_cb,
        .name = "lms_poll",
    };
    esp_timer_create(&timer_args, &s_poll_timer);
    esp_timer_start_periodic(s_poll_timer, LMS_POLL_INTERVAL_US);

    logger_log(ESP_LOG_INFO, TAG, "lms_metadata: conectando em %s (queue_id=%s)", uri, s_queue_id);
}

void lms_metadata_get(lms_metadata_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_mutex == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_metadata;
    xSemaphoreGive(s_mutex);
}

bool lms_metadata_is_configured(void)
{
    return s_configured;
}

bool lms_metadata_auth_failed(void)
{
    return s_auth_failed;
}
