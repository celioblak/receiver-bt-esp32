#include "web_server.h"

#include <stdio.h>
#include <string.h>

#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "logger.h"
#include "relay_control.h"
#include "storage.h"
#include "wifi_manager.h"

#include "cJSON.h"
#include "esp_bt_device.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

static const char *TAG = "web_server";

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    free(text);
    cJSON_Delete(root);
    return err;
}

/* Lê o corpo da requisição (até max_len-1 bytes) e devolve um cJSON, ou
 * NULL respondendo 400 se o corpo estiver vazio/inválido. Chamador libera
 * o cJSON retornado. */
static cJSON *recv_json_body(httpd_req_t *req, char *buf, size_t max_len)
{
    size_t to_read = req->content_len < (max_len - 1) ? req->content_len : (max_len - 1);
    int len = httpd_req_recv(req, buf, to_read);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo da requisicao vazio");
        return NULL;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalido");
        return NULL;
    }
    return root;
}

/* -------------------------------------------------------------------------
 * API REST
 * ------------------------------------------------------------------------- */

static esp_err_t api_status_get(httpd_req_t *req)
{
    bt_audio_status_t bt;
    bt_audio_get_status(&bt);

    char device_name[32];
    if (storage_get_str(NVS_KEY_DEVICE_NAME, device_name, sizeof(device_name)) != ESP_OK) {
        strlcpy(device_name, FW_DEVICE_NAME_DEFAULT, sizeof(device_name));
    }

    char own_mac[18] = "";
    const uint8_t *bda = esp_bt_dev_get_address();
    if (bda != NULL) {
        snprintf(own_mac, sizeof(own_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    }

    char ip[16];
    wifi_manager_get_ip_str(ip, sizeof(ip));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "bt_connected", bt.connected);
    cJSON_AddStringToObject(root, "device_name", device_name);
    cJSON_AddStringToObject(root, "device_mac", own_mac);
    cJSON_AddStringToObject(root, "track", bt.title);
    cJSON_AddStringToObject(root, "artist", bt.artist);
    cJSON_AddStringToObject(root, "album", bt.album);
    cJSON_AddBoolToObject(root, "playing", bt.playing);
    cJSON_AddBoolToObject(root, "amplifier", relay_control_is_on());
    cJSON_AddNumberToObject(root, "volume", audio_codec_get_volume());
    cJSON_AddStringToObject(root, "wifi_ip", ip);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    return send_json(req, root);
}

static esp_err_t api_config_get(httpd_req_t *req)
{
    char device_name[32];
    char wifi_ssid[33];
    int32_t relay_timeout = DEFAULT_RELAY_TIMEOUT_S;

    if (storage_get_str(NVS_KEY_DEVICE_NAME, device_name, sizeof(device_name)) != ESP_OK) {
        strlcpy(device_name, FW_DEVICE_NAME_DEFAULT, sizeof(device_name));
    }
    if (storage_get_str(NVS_KEY_WIFI_SSID, wifi_ssid, sizeof(wifi_ssid)) != ESP_OK) {
        wifi_ssid[0] = '\0';
    }
    storage_get_i32(NVS_KEY_RELAY_TIMEOUT, &relay_timeout, DEFAULT_RELAY_TIMEOUT_S);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", device_name);
    cJSON_AddStringToObject(root, "wifi_ssid", wifi_ssid);
    /* wifi_pass nunca é devolvida por segurança — só aceita na escrita (POST). */
    cJSON_AddNumberToObject(root, "relay_timeout_s", relay_timeout);

    return send_json(req, root);
}

static esp_err_t api_config_post(httpd_req_t *req)
{
    char buf[512];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *item;
    bool wifi_changed = false;

    if ((item = cJSON_GetObjectItem(root, "device_name")) && cJSON_IsString(item)) {
        storage_set_str(NVS_KEY_DEVICE_NAME, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_ssid")) && cJSON_IsString(item)) {
        storage_set_str(NVS_KEY_WIFI_SSID, item->valuestring);
        wifi_changed = true;
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_pass")) && cJSON_IsString(item)) {
        storage_set_str(NVS_KEY_WIFI_PASS, item->valuestring);
        wifi_changed = true;
    }
    if ((item = cJSON_GetObjectItem(root, "relay_timeout_s")) && cJSON_IsNumber(item)) {
        storage_set_i32(NVS_KEY_RELAY_TIMEOUT, item->valueint);
    }
    cJSON_Delete(root);

    logger_log(ESP_LOG_INFO, TAG, "Configuracao atualizada via API");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message",
                             wifi_changed ? "Salvo. Reinicie o dispositivo para aplicar as novas credenciais de Wi-Fi."
                                          : "Salvo.");
    return send_json(req, resp);
}

static esp_err_t api_volume_post(httpd_req_t *req)
{
    char buf[64];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *item = cJSON_GetObjectItem(root, "volume");
    if (item == NULL || !cJSON_IsNumber(item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "campo \"volume\" ausente ou invalido");
        return ESP_FAIL;
    }

    audio_codec_set_volume(item->valueint);
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "volume", audio_codec_get_volume());
    return send_json(req, resp);
}

static esp_err_t api_logs_get(httpd_req_t *req)
{
    static log_entry_t entries[LOGGER_MAX_ENTRIES];
    size_t n = logger_get_entries(entries, LOGGER_MAX_ENTRIES);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "timestamp_ms", (double)entries[i].timestamp_ms);
        cJSON_AddNumberToObject(item, "level", entries[i].level);
        cJSON_AddStringToObject(item, "msg", entries[i].msg);
        cJSON_AddItemToArray(arr, item);
    }

    return send_json(req, arr);
}

/* -------------------------------------------------------------------------
 * Arquivos estáticos (interface web) — servidos direto do SPIFFS
 * ------------------------------------------------------------------------- */

static const char *content_type_for(const char *path)
{
    if (strstr(path, ".html")) return "text/html";
    if (strstr(path, ".css")) return "text/css";
    if (strstr(path, ".js")) return "application/javascript";
    if (strstr(path, ".json")) return "application/json";
    if (strstr(path, ".ico")) return "image/x-icon";
    return "text/plain";
}

static esp_err_t static_file_get(httpd_req_t *req)
{
    char filepath[160] = "/spiffs";

    if (strcmp(req->uri, "/") == 0) {
        strlcat(filepath, "/index.html", sizeof(filepath));
    } else {
        strlcat(filepath, req->uri, sizeof(filepath));
    }

    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type_for(filepath));

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Inicialização
 * ------------------------------------------------------------------------- */

static esp_err_t mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 6,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao montar SPIFFS: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(conf.partition_label, &total, &used);
    logger_log(ESP_LOG_INFO, TAG, "SPIFFS montado: %u/%u bytes usados", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

void web_server_start(void)
{
    if (mount_spiffs() != ESP_OK) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "falha ao iniciar o servidor HTTP");
        return;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = api_status_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = api_config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = api_config_post},
        {.uri = "/api/volume", .method = HTTP_POST, .handler = api_volume_post},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_get},
        {.uri = "/*", .method = HTTP_GET, .handler = static_file_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    logger_log(ESP_LOG_INFO, TAG, "Servidor web iniciado na porta %d", config.server_port);
}
