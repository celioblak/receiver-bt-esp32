#include "web_server.h"

#include <stdio.h>
#include <string.h>

#include "audio_agc.h"
#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "dlna_renderer.h"
#include "logger.h"
#include "ota_manager.h"
#include "pairing.h"
#include "relay_control.h"
#include "storage.h"
#include "wifi_manager.h"

#include "cJSON.h"
#include "esp_bt_device.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    /* Nome do dispositivo Bluetooth conectado: procurado no histórico de
     * pareamento pelo MAC (a pilha não guarda o nome remoto em lugar
     * nenhum acessível fora do evento de pareamento em si). */
    char bt_remote_name[32] = "";
    if (bt.connected && bt.remote_mac[0] != '\0') {
        pairing_device_t history[PAIRING_HISTORY_MAX];
        size_t n = pairing_get_history(history, PAIRING_HISTORY_MAX);
        for (size_t i = 0; i < n; i++) {
            char mac_str[18];
            pairing_format_mac(history[i].mac, mac_str, sizeof(mac_str));
            if (strcmp(mac_str, bt.remote_mac) == 0) {
                strlcpy(bt_remote_name, history[i].name, sizeof(bt_remote_name));
                break;
            }
        }
    }

    /* BT sempre tem prioridade (mesma regra do audio em si, ver
     * dlna_should_abort() em dlna_renderer.c) -- so mostra o que o DLNA
     * esta tocando quando nao ha celular conectado. */
    const char *track = bt.title;
    const char *artist = bt.artist;
    const char *album = bt.album;
    bool playing = bt.playing;
    dlna_status_t dlna = {0};
    dlna_renderer_get_status(&dlna);
    if (!bt.connected && dlna.playing) {
        track = dlna.track;
        artist = dlna.artist;
        album = dlna.album;
        playing = true;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "bt_connected", bt.connected);
    cJSON_AddStringToObject(root, "device_name", device_name);
    cJSON_AddStringToObject(root, "device_mac", own_mac);
    cJSON_AddStringToObject(root, "bt_remote_mac", bt.remote_mac);
    cJSON_AddStringToObject(root, "bt_remote_name", bt_remote_name);
    cJSON_AddStringToObject(root, "track", track);
    cJSON_AddStringToObject(root, "artist", artist);
    cJSON_AddStringToObject(root, "album", album);
    cJSON_AddBoolToObject(root, "playing", playing);
    /* DLNA fica ativo independente do BT estar conectado (BT so tem
     * prioridade pra decidir o que sai no I2S, ver dlna_should_abort()) --
     * exposto separado pra pagina web mostrar "quem esta conectado via
     * DLNA" mesmo quando o audio de fato tocando e via Bluetooth. "state"
     * e "subscribed" (nao so um bool "active") pra distinguir pausado de
     * parado/nunca conectado -- client_ip/agent sozinhos ficavam gravados
     * da ultima acao SOAP mesmo depois de pausar, o que fazia a pagina
     * mostrar "conectado" e "inativo" ao mesmo tempo, contraditorio. */
    cJSON_AddStringToObject(root, "dlna_state", dlna.state);
    cJSON_AddBoolToObject(root, "dlna_subscribed", dlna.subscribed);
    cJSON_AddStringToObject(root, "dlna_client_ip", dlna.client_ip);
    cJSON_AddStringToObject(root, "dlna_client_agent", dlna.client_agent);
    cJSON_AddStringToObject(root, "dlna_position", dlna.position);
    cJSON_AddStringToObject(root, "dlna_duration", dlna.duration);
    cJSON_AddBoolToObject(root, "amplifier", relay_control_is_on());
    /* API/UI expoem 0-100 pro usuario -- a precisao fina de 0-VOLUME_STEPS
     * (200, ver config.h) e so um detalhe interno da curva de audio_codec.c,
     * nao precisa vazar pra fora (usuario pediu explicitamente: "nao
     * importa nossa formula matematica interna"). */
    cJSON_AddNumberToObject(root, "volume", (audio_codec_get_volume() * 100 + VOLUME_STEPS / 2) / VOLUME_STEPS);
    cJSON_AddBoolToObject(root, "agc_enabled", audio_agc_is_enabled());
    cJSON_AddNumberToObject(root, "agc_gain", audio_agc_get_current_gain());
    cJSON_AddNumberToObject(root, "agc_target", audio_agc_get_target());
    cJSON_AddNumberToObject(root, "agc_mode", audio_agc_get_mode());
    cJSON_AddStringToObject(root, "wifi_ip", ip);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(root, "bt_discoverable", bt_audio_get_discoverable());
    cJSON_AddBoolToObject(root, "bt_require_pin", bt_audio_get_require_pin());
    cJSON_AddStringToObject(root, "pending_pin_mac", bt.pending_pin_mac);
    cJSON_AddStringToObject(root, "pending_pin_code", bt.pending_pin_code);

    return send_json(req, root);
}

static esp_err_t api_config_get(httpd_req_t *req)
{
    char device_name[32];
    char wifi_ssid[33];
    char mqtt_host[64];
    char mqtt_user[32];
    int32_t relay_timeout = DEFAULT_RELAY_TIMEOUT_S;
    int32_t mqtt_port = 1883;

    if (storage_get_str(NVS_KEY_DEVICE_NAME, device_name, sizeof(device_name)) != ESP_OK) {
        strlcpy(device_name, FW_DEVICE_NAME_DEFAULT, sizeof(device_name));
    }
    if (storage_get_str(NVS_KEY_WIFI_SSID, wifi_ssid, sizeof(wifi_ssid)) != ESP_OK) {
        wifi_ssid[0] = '\0';
    }
    if (storage_get_str(NVS_KEY_MQTT_HOST, mqtt_host, sizeof(mqtt_host)) != ESP_OK) {
        mqtt_host[0] = '\0';
    }
    if (storage_get_str(NVS_KEY_MQTT_USER, mqtt_user, sizeof(mqtt_user)) != ESP_OK) {
        mqtt_user[0] = '\0';
    }
    storage_get_i32(NVS_KEY_RELAY_TIMEOUT, &relay_timeout, DEFAULT_RELAY_TIMEOUT_S);
    storage_get_i32(NVS_KEY_MQTT_PORT, &mqtt_port, 1883);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", device_name);
    cJSON_AddStringToObject(root, "wifi_ssid", wifi_ssid);
    /* wifi_pass e mqtt_pass nunca são devolvidas por segurança — só aceitas na escrita (POST). */
    cJSON_AddNumberToObject(root, "relay_timeout_s", relay_timeout);
    cJSON_AddStringToObject(root, "mqtt_host", mqtt_host);
    cJSON_AddNumberToObject(root, "mqtt_port", mqtt_port);
    cJSON_AddStringToObject(root, "mqtt_user", mqtt_user);
    cJSON_AddBoolToObject(root, "bt_discoverable", bt_audio_get_discoverable());
    cJSON_AddBoolToObject(root, "bt_require_pin", bt_audio_get_require_pin());

    return send_json(req, root);
}

static esp_err_t api_config_post(httpd_req_t *req)
{
    /* 1024 (nao 512): o token JWT de longa duracao do Music Assistant
     * (ma_token) sozinho ja passa de 300 bytes -- precisa de folga extra
     * pra caber junto com os outros campos quando a tela de config manda
     * tudo de uma vez. */
    char buf[1024];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *item;
    bool reboot_needed = false;

    if ((item = cJSON_GetObjectItem(root, "device_name")) && cJSON_IsString(item)) {
        storage_set_str(NVS_KEY_DEVICE_NAME, item->valuestring);
        wifi_manager_set_mdns_hostname(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_ssid")) && cJSON_IsString(item)) {
        storage_set_str(NVS_KEY_WIFI_SSID, item->valuestring);
        reboot_needed = true;
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_pass")) && cJSON_IsString(item)) {
        storage_set_str(NVS_KEY_WIFI_PASS, item->valuestring);
        reboot_needed = true;
    }
    if ((item = cJSON_GetObjectItem(root, "relay_timeout_s")) && cJSON_IsNumber(item)) {
        storage_set_i32(NVS_KEY_RELAY_TIMEOUT, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "mqtt_host")) && cJSON_IsString(item)) {
        storage_set_str(NVS_KEY_MQTT_HOST, item->valuestring);
        reboot_needed = true;
    }
    if ((item = cJSON_GetObjectItem(root, "mqtt_port")) && cJSON_IsNumber(item)) {
        storage_set_i32(NVS_KEY_MQTT_PORT, item->valueint);
        reboot_needed = true;
    }
    if ((item = cJSON_GetObjectItem(root, "mqtt_user")) && cJSON_IsString(item)) {
        storage_set_str(NVS_KEY_MQTT_USER, item->valuestring);
        reboot_needed = true;
    }
    if ((item = cJSON_GetObjectItem(root, "mqtt_pass")) && cJSON_IsString(item)) {
        storage_set_str(NVS_KEY_MQTT_PASS, item->valuestring);
        reboot_needed = true;
    }
    if ((item = cJSON_GetObjectItem(root, "bt_discoverable")) && cJSON_IsBool(item)) {
        bt_audio_set_discoverable(cJSON_IsTrue(item));
    }
    if ((item = cJSON_GetObjectItem(root, "bt_require_pin")) && cJSON_IsBool(item)) {
        bt_audio_set_require_pin(cJSON_IsTrue(item));
    }
    cJSON_Delete(root);

    logger_log(ESP_LOG_INFO, TAG, "Configuracao atualizada via API");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message",
                             reboot_needed ? "Salvo. Reinicie o dispositivo para aplicar as mudancas de Wi-Fi/MQTT."
                                           : "Salvo.");
    return send_json(req, resp);
}

static esp_err_t api_system_restart_post(httpd_req_t *req)
{
    logger_log(ESP_LOG_INFO, TAG, "Reinicio solicitado via API");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Reiniciando...\"}");

    /* da tempo da resposta HTTP sair antes de reiniciar (mesmo padrao do OTA) */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; /* nunca chega aqui */
}

static esp_err_t api_system_beep_post(httpd_req_t *req)
{
    /* Confirmado na pratica (2x): disparar o bipe enquanto o BT esta tocando
     * de verdade trava o dispositivo -- ainda nao temos certeza total do
     * mecanismo exato (mutex do I2S deveria bastar, mas o travamento se
     * repetiu mesmo depois dele), entao a defesa mais segura agora e
     * simplesmente recusar o bipe nesse cenario em vez de arriscar outro
     * travamento. O bipe e uma ferramenta de diagnostico de hardware, nao
     * precisa funcionar durante playback real. */
    bt_audio_status_t bt;
    bt_audio_get_status(&bt);
    if (bt.connected) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"audio ja em uso (BT tocando)\"}");
        return ESP_OK;
    }

    logger_log(ESP_LOG_INFO, TAG, "Bipe de teste solicitado via API");
    audio_codec_play_test_tone(); /* bloqueia ~300ms -- inofensivo, so atrasa a resposta HTTP um pouco */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
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

    /* API/UI usam 0-100; convertido aqui pra escala interna 0-VOLUME_STEPS
     * (200, ver config.h) que audio_codec.c usa pra granularidade fina da
     * curva. Se o AGC estiver ligado, a próxima iteração da agc_task já lê
     * este novo valor via audio_codec_get_volume() — não precisa
     * sincronizar nada aqui. */
    audio_codec_set_volume((item->valueint * VOLUME_STEPS) / 100);
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "volume", (audio_codec_get_volume() * 100 + VOLUME_STEPS / 2) / VOLUME_STEPS);
    return send_json(req, resp);
}

static esp_err_t api_media_post(httpd_req_t *req)
{
    char buf[64];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *item = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "\"cmd\" deve ser play, pause, playpause, stop, next ou previous");
        return ESP_FAIL;
    }

    /* BT primeiro (mesma prioridade do resto do projeto); se nao ha celular
     * conectado, tenta a fonte DLNA -- antes o comando ia so pro bt_audio e
     * sumia sem efeito nenhum quando quem tocava era o DLNA. */
    bt_audio_status_t bt;
    bt_audio_get_status(&bt);
    esp_err_t err;
    if (bt.connected) {
        err = bt_audio_media_control(item->valuestring);
    } else {
        err = dlna_renderer_media_control(item->valuestring);
    }

    if (err == ESP_ERR_NOT_SUPPORTED) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "pular faixa nao e possivel via DLNA: a fila pertence ao control point "
                             "(use o Music Assistant para trocar de faixa)");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "comando invalido ou nenhuma fonte de audio ativa");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

static esp_err_t api_agc_post(httpd_req_t *req)
{
    char buf[128];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "target")) && cJSON_IsNumber(item)) {
        audio_agc_set_target((int8_t)item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "mode")) && cJSON_IsNumber(item)) {
        audio_agc_set_mode((uint8_t)item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(item)) {
        audio_agc_enable(cJSON_IsTrue(item));
    }
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "agc_enabled", audio_agc_is_enabled());
    cJSON_AddNumberToObject(resp, "agc_target", audio_agc_get_target());
    cJSON_AddNumberToObject(resp, "agc_mode", audio_agc_get_mode());
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

static esp_err_t api_devices_get(httpd_req_t *req)
{
    pairing_device_t history[PAIRING_HISTORY_MAX];
    size_t n = pairing_get_history(history, PAIRING_HISTORY_MAX);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        char mac_str[18];
        pairing_format_mac(history[i].mac, mac_str, sizeof(mac_str));

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", mac_str);
        cJSON_AddStringToObject(item, "name", history[i].name);
        cJSON_AddNumberToObject(item, "last_seen_ms", (double)history[i].last_seen_ms);
        cJSON_AddBoolToObject(item, "allowed", pairing_is_allowed(history[i].mac));
        cJSON_AddItemToArray(arr, item);
    }

    return send_json(req, arr);
}

static esp_err_t api_wifi_scan_get(httpd_req_t *req)
{
    wifi_manager_scan_result_t results[WIFI_MANAGER_SCAN_MAX];
    size_t n = wifi_manager_scan(results, WIFI_MANAGER_SCAN_MAX);

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", results[i].rssi);
        cJSON_AddBoolToObject(item, "secure", results[i].secure);
        cJSON_AddItemToArray(arr, item);
    }

    return send_json(req, arr);
}

/* Liga o "descobrivel" do Bluetooth so por um tempo limitado (padrao 180s se
 * "duration_s" nao vier no corpo) -- ver bt_audio_enable_discoverable_
 * temporary(). Existe pra nao deixar a varredura periodica de radio do BT
 * ligada o tempo todo (disputa CPU/radio com a decodificacao de audio),
 * so durante o tempo real de parear um aparelho novo. */
static esp_err_t api_bt_pairing_mode_post(httpd_req_t *req)
{
    /* recv_json_body ja manda a resposta de erro sozinha se o corpo vier
     * vazio/invalido -- NAO mandar outra resposta nesse caso (corpo minimo
     * esperado do chamador: "{}" pra usar so o padrao de 180s). */
    char buf[128];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }
    uint32_t duration_s = 180;
    cJSON *item = cJSON_GetObjectItem(root, "duration_s");
    if (cJSON_IsNumber(item) && item->valueint > 0) {
        duration_s = (uint32_t)item->valueint;
    }
    cJSON_Delete(root);
    bt_audio_enable_discoverable_temporary(duration_s);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "duration_s", duration_s);
    return send_json(req, resp);
}

static esp_err_t api_pair_post(httpd_req_t *req)
{
    char buf[256];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *mac_item = cJSON_GetObjectItem(root, "mac");
    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    uint8_t mac[6];

    if (!cJSON_IsString(mac_item) || !cJSON_IsString(action_item) ||
        !pairing_parse_mac(mac_item->valuestring, mac)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "\"mac\" ou \"action\" invalidos");
        return ESP_FAIL;
    }

    const char *action = action_item->valuestring;
    if (strcmp(action, "allow") == 0) {
        pairing_set_allowed(mac, true);
    } else if (strcmp(action, "block") == 0) {
        pairing_set_allowed(mac, false);
    } else if (strcmp(action, "remove") == 0) {
        pairing_remove_from_history(mac);
    } else if (strcmp(action, "forget") == 0) {
        /* Diferente de "remove": tira o bond no controlador BT e
         * desconecta agora, nao so limpa o historico -- sem isso o
         * celular reconectava sozinho de novo (link key salva). */
        bt_audio_forget_device(mac);
    } else if (strcmp(action, "disconnect") == 0) {
        /* Diferente de "forget": so derruba a conexao, mantem o
         * pareamento -- o dispositivo pode reconectar depois normalmente. */
        bt_audio_disconnect_device(mac);
    } else {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action deve ser allow, block, remove, forget ou disconnect");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
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
    config.max_uri_handlers = 20; /* 16 rotas hoje -- folga pra novas sem esbarrar no limite de novo */
    config.stack_size = 8192; /* /ota escreve na flash — folga extra de pilha */
    config.recv_wait_timeout = 10;

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
        {.uri = "/api/media", .method = HTTP_POST, .handler = api_media_post},
        {.uri = "/api/agc", .method = HTTP_POST, .handler = api_agc_post},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_get},
        {.uri = "/api/devices", .method = HTTP_GET, .handler = api_devices_get},
        {.uri = "/api/pair", .method = HTTP_POST, .handler = api_pair_post},
        {.uri = "/api/bt/pairing_mode", .method = HTTP_POST, .handler = api_bt_pairing_mode_post},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_get},
        {.uri = "/api/system/restart", .method = HTTP_POST, .handler = api_system_restart_post},
        {.uri = "/api/system/beep", .method = HTTP_POST, .handler = api_system_beep_post},
        /* GET tambem, de proposito -- diagnostico pra acionar direto da
         * barra de enderecos do navegador (util remotamente, sem precisar
         * de curl/Postman a mao). */
        {.uri = "/api/system/beep", .method = HTTP_GET, .handler = api_system_beep_post},
        {.uri = "/ota", .method = HTTP_POST, .handler = ota_manager_upload_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = static_file_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    logger_log(ESP_LOG_INFO, TAG, "Servidor web iniciado na porta %d", config.server_port);
}
