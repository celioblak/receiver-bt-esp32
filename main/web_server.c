#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_agc.h"
#include "audio_codec.h"
#include "audio_source.h"
#include "bt_audio.h"
#include "config.h"

#include "driver/gpio.h"
#include "dlna_renderer.h"
#include "es8388.h"
#include "logger.h"
#include "ota_manager.h"
#include "pairing.h"
#include "relay_control.h"
#include "storage.h"
#include "wifi_manager.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_bt_device.h"
#include "esp_heap_caps.h"
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
    /* Mostra os metadados do DLNA quando ha faixa CARREGADA -- tocando OU
     * pausada. Antes a condicao era dlna.playing, que o Pause zera: a faixa
     * desaparecia da pagina ao pausar, o que e incoerente (pausado nao e
     * parado, a faixa continua carregada). O campo "playing" segue refletindo
     * so a reproducao de fato. */
    const bool dlna_tem_faixa = (strcmp(dlna.state, "playing") == 0 ||
                                 strcmp(dlna.state, "paused") == 0);
    if (!bt.connected && dlna_tem_faixa) {
        track = dlna.track;
        artist = dlna.artist;
        album = dlna.album;
        playing = dlna.playing;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "audio_source", audio_source_name(audio_source_get()));
    /* Medidor do mic (pico 0-32767 do ultimo bloco) -- serve pra regular o
     * ganho/limiar olhando o numero enquanto fala/canta. */
cJSON_AddStringToObject(root, "mic_adc", audio_codec_get_mic_adc_estado());
    cJSON_AddNumberToObject(root, "mic_peak", audio_codec_get_mic_peak());
    cJSON_AddNumberToObject(root, "mic_clip", audio_codec_mic_get_clip());
    cJSON_AddNumberToObject(root, "mic_limit_hits", audio_codec_mic_get_limit_hits());
    cJSON_AddBoolToObject(root, "bt_connected", bt.connected);
    /* Qualidade dos dois enlaces de radio.
     *
     * O pedido do BT e ASSINCRONO: a resposta chega num evento do GAP e
     * aparece na leitura de status seguinte, entao logo depois de conectar a
     * primeira costuma vir sem valor. Por isso o campo so e publicado quando
     * ja houve medicao -- melhor ausente que mentindo zero. */
    bt_audio_request_rssi();
    if (bt.connected && bt.rssi_valido) {
        cJSON_AddNumberToObject(root, "bt_rssi_delta", bt.rssi_delta);
    }
    cJSON_AddNumberToObject(root, "bt_rssi_interval_s", bt_audio_get_rssi_interval());
    int wifi_rssi = 0;
    if (wifi_manager_get_rssi(&wifi_rssi)) {
        cJSON_AddNumberToObject(root, "wifi_rssi", wifi_rssi);
    }
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
    /* Segundos restantes da janela de pareamento. 0 com bt_discoverable=true
     * significa visibilidade PERMANENTE (nao ha prazo pra contar). */
    cJSON_AddNumberToObject(root, "bt_discoverable_remaining_s",
                            bt_audio_get_discoverable_remaining_s());
    /* >0 significa lista de autorizados ativa: SO os dessa lista conseguem
     * parear, entao abrir a janela nao basta pra um aparelho novo (ver
     * pairing_is_allowed). A pagina usa isso pra avisar. */
    cJSON_AddNumberToObject(root, "bt_allowed_count", (double)pairing_get_allowed_count());

    /* SHA256 do ELF em execucao (8 primeiros bytes). E a unica prova confiavel
     * de QUAL firmware esta rodando depois de um OTA: o auto-reset apos o
     * upload falha as vezes nesta placa, e "data de compilacao" ja enganou --
     * bate mesmo quando o binario velho continua ativo. Compare com
     * `sha256sum .pio/build/esp32-a1s/firmware.elf` (mesmos 16 digitos). */
    const esp_app_desc_t *desc = esp_app_get_description();
    char sha[17];
    for (int i = 0; i < 8; i++) {
        sprintf(sha + i * 2, "%02x", desc->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(root, "fw_sha", sha);

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
    cJSON_AddBoolToObject(root, "mic_enabled", audio_codec_mic_is_enabled());
    cJSON_AddBoolToObject(root, "mic_auto_gate", audio_codec_get_mic_auto_gate());
    cJSON_AddNumberToObject(root, "mic_gain", audio_codec_get_mic_gain());
    cJSON_AddNumberToObject(root, "mic_treble", audio_codec_get_mic_treble());
    cJSON_AddNumberToObject(root, "mic_limiter", audio_codec_get_mic_limiter());
    cJSON_AddNumberToObject(root, "mic_gate_level", audio_codec_get_mic_gate_threshold());
    cJSON_AddBoolToObject(root, "relay_active_low", relay_control_get_active_low());
    cJSON_AddBoolToObject(root, "relay_open_drain", relay_control_get_open_drain());
    cJSON_AddNumberToObject(root, "bt_rssi_interval_s", bt_audio_get_rssi_interval());
    cJSON_AddNumberToObject(root, "mic_input", audio_codec_mic_get_input());
    cJSON_AddStringToObject(root, "mic_input_nome", audio_codec_mic_get_input_name());
    cJSON_AddBoolToObject(root, "pairing_lock", pairing_get_lock_mode());

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
    if ((item = cJSON_GetObjectItem(root, "mic_enabled")) && cJSON_IsBool(item)) {
        /* Aplica NA HORA, sem reiniciar (2026-08-29). Antes so gravava na NVS
         * e exigia reboot, porque o canal I2S RX so nascia no boot.
         *
         * Ligar aqui cria o canal RX neste instante e ja informa em que estado
         * o ADC subiu (ver mic_adc no /api/status). Como o RX e a unica coisa
         * que sobe instavel nesta placa, isso muda o jogo: se o microfone
         * subir travado ou chiando, e so desligar e ligar de novo pra tentar
         * outra vez -- sem reiniciar o aparelho e sem derrubar a musica.
         * Antes a unica saida era reiniciar tudo, e chegou a levar 5 reinicios
         * seguidos. audio_codec_mic_set_enabled() ja persiste na NVS. */
        audio_codec_mic_set_enabled(cJSON_IsTrue(item));
    }
    if ((item = cJSON_GetObjectItem(root, "mic_auto_gate")) && cJSON_IsBool(item)) {
        audio_codec_set_mic_auto_gate(cJSON_IsTrue(item));
    }
    /* Ganho/limiar do mic: aplicam na hora (sem reiniciar) de proposito --
     * servem justamente pra afinar cantando. */
    if ((item = cJSON_GetObjectItem(root, "mic_gain")) && cJSON_IsNumber(item)) {
        audio_codec_set_mic_gain(item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "mic_treble")) && cJSON_IsNumber(item)) {
        audio_codec_set_mic_treble(item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "mic_limiter")) && cJSON_IsNumber(item)) {
        audio_codec_set_mic_limiter(item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "mic_gate_level")) && cJSON_IsNumber(item)) {
        audio_codec_set_mic_gate_threshold(item->valueint);
    }
    /* Entrada analogica do codec (indice de es8388_mic_input_t). Aplica na
     * hora -- e so uma escrita de I2C, o I2S nem e tocado. */
    if ((item = cJSON_GetObjectItem(root, "mic_input")) && cJSON_IsNumber(item)) {
        audio_codec_mic_set_input(item->valueint);
    }
    /* Polaridade do modulo de rele -- aplica na hora, sem reiniciar. */
    if ((item = cJSON_GetObjectItem(root, "relay_active_low")) && cJSON_IsBool(item)) {
        relay_control_set_active_low(cJSON_IsTrue(item));
    }
    if ((item = cJSON_GetObjectItem(root, "relay_open_drain")) && cJSON_IsBool(item)) {
        relay_control_set_open_drain(cJSON_IsTrue(item));
    }
    /* Intervalo da medicao de sinal do BT, em segundos (0 desliga). Existe
     * para ISOLAR: a medicao deixou o aparelho instavel com um celular
     * conectado, e sem poder liga-la e desliga-la em runtime nao da para
     * saber se a culpa e dela nem a partir de que frequencia incomoda. */
    if ((item = cJSON_GetObjectItem(root, "bt_rssi_interval_s")) && cJSON_IsNumber(item)) {
        bt_audio_set_rssi_interval(item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "pairing_lock")) && cJSON_IsBool(item)) {
        pairing_set_lock_mode(cJSON_IsTrue(item));
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

/* Escapa uma string para caber dentro de aspas em JSON. Só os casos que a RFC
 * exige -- aspas, barra invertida e controles. */
static void json_escape_into(char *out, size_t out_size, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < out_size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (c == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (c == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (c < 0x20) {
            o += snprintf(out + o, out_size - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c; /* UTF-8 passa direto */
        }
    }
    out[o] = '\0';
}

/* Responde em PEDACOS, uma entrada por vez, em vez de montar o array inteiro
 * com cJSON e imprimir num buffer unico.
 *
 * MOTIVO (bug real, reproduzido 3/3): com o buffer de log cheio (100 entradas
 * x 128 bytes) o JSON passa de ~17KB, e o cJSON aloca da RAM INTERNA -- onde
 * sobram ~20KB neste firmware, e o print ainda realoca crescendo, chegando a
 * pedir o dobro. O endpoint entao devolvia 0 byte e prendia a conexao ate o
 * timeout do cliente, enquanto /api/status e /api/devices (JSON pequeno)
 * respondiam normalmente. Funcionava logo apos o boot, com poucas entradas, e
 * quebrava depois que o buffer enchia -- o que fazia parecer intermitente.
 * Em pedacos, o pico de memoria e o de UMA entrada. */
/* Buffers do endpoint de log em PSRAM, NAO em RAM interna.
 *
 * MOTIVO (medido ao vivo): a copia das entradas sozinha eram ~14,4KB
 * (100 x sizeof(log_entry_t)) num `static` dentro da funcao -- ou seja, .bss,
 * RAM interna. E RAM interna e o recurso critico aqui: no boot sobram ~21KB, e
 * conectar um aparelho Bluetooth (A2DP+AVRCP) consome ~11KB, deixando ~9KB.
 * Nesse ponto o WiFi passa a falhar alocacao (`wifi:m f null` no log), e HTTP
 * e MQTT caem juntos -- o dispositivo continua vivo, mas a pagina web fica
 * inacessivel enquanto o Bluetooth estiver conectado (sintoma reportado como
 * "crash"). Tirar esses ~16KB da RAM interna e o maior ganho isolado
 * disponivel. Alocado sob demanda, na primeira chamada. */
static log_entry_t *s_log_copy = NULL;
static char *s_log_esc = NULL;
static char *s_log_piece = NULL;
#define LOG_ESC_SIZE   (LOGGER_MSG_MAX_LEN * 6 + 8)
#define LOG_PIECE_SIZE (LOGGER_MSG_MAX_LEN * 6 + 96)

static esp_err_t api_logs_get(httpd_req_t *req)
{
    if (s_log_copy == NULL) {
        s_log_copy = heap_caps_malloc(sizeof(log_entry_t) * LOGGER_MAX_ENTRIES, MALLOC_CAP_SPIRAM);
        s_log_esc = heap_caps_malloc(LOG_ESC_SIZE, MALLOC_CAP_SPIRAM);
        s_log_piece = heap_caps_malloc(LOG_PIECE_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (s_log_copy == NULL || s_log_esc == NULL || s_log_piece == NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    log_entry_t *entries = s_log_copy;
    char *esc = s_log_esc;
    char *piece = s_log_piece;
    size_t n = logger_get_entries(entries, LOGGER_MAX_ENTRIES);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);

    for (size_t i = 0; i < n; i++) {
        json_escape_into(esc, LOG_ESC_SIZE, entries[i].msg);
        /* LOG_PIECE_SIZE, nao sizeof(piece): agora e ponteiro, e sizeof daria
         * 4 bytes silenciosamente. */
        int len = snprintf(piece, LOG_PIECE_SIZE,
                           "%s{\"timestamp_ms\":%llu,\"level\":%d,\"msg\":\"%s\"}",
                           i ? "," : "",
                           (unsigned long long)entries[i].timestamp_ms,
                           (int)entries[i].level, esc);
        if (len > 0) {
            if ((size_t)len >= LOG_PIECE_SIZE) {
                len = (int)LOG_PIECE_SIZE - 1; /* nunca enviar alem do buffer */
            }
            httpd_resp_send_chunk(req, piece, len);
        }
    }

    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0); /* termina o chunked encoding */
    return ESP_OK;
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

    /* Um mac autorizado ou bloqueado pode nao ter (ou ter perdido) entrada
     * no historico -- sem isso ficava invisivel na interface e sem jeito de
     * desfazer (ver pairing_get_blocked_list() em pairing.c). Mescla aqui,
     * pulando quem ja apareceu acima pelo historico. */
    uint8_t extra_macs[PAIRING_MAX_ALLOWED][6];
    size_t extra_n = pairing_get_allowed_list(extra_macs, PAIRING_MAX_ALLOWED);
    uint8_t blocked_macs[PAIRING_MAX_ALLOWED][6];
    size_t blocked_n = pairing_get_blocked_list(blocked_macs, PAIRING_MAX_ALLOWED);

    for (int pass = 0; pass < 2; pass++) {
        uint8_t (*list)[6] = (pass == 0) ? extra_macs : blocked_macs;
        size_t list_n = (pass == 0) ? extra_n : blocked_n;
        for (size_t i = 0; i < list_n; i++) {
            bool already_listed = false;
            for (size_t j = 0; j < n; j++) {
                if (memcmp(history[j].mac, list[i], 6) == 0) {
                    already_listed = true;
                    break;
                }
            }
            if (already_listed) {
                continue;
            }
            char mac_str[18];
            pairing_format_mac(list[i], mac_str, sizeof(mac_str));
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "mac", mac_str);
            cJSON_AddStringToObject(item, "name", "");
            cJSON_AddNumberToObject(item, "last_seen_ms", 0);
            cJSON_AddBoolToObject(item, "allowed", pairing_is_allowed(list[i]));
            cJSON_AddItemToArray(arr, item);
        }
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
    uint32_t duration_s = DEFAULT_BT_PAIRING_WINDOW_S;
    cJSON *item = cJSON_GetObjectItem(root, "duration_s");
    if (cJSON_IsNumber(item) && item->valueint > 0) {
        duration_s = (uint32_t)item->valueint;
    }
    /* "stop": encerra a janela antes do prazo (botao "Encerrar agora"). */
    cJSON *stop = cJSON_GetObjectItem(root, "stop");
    bool encerrar = cJSON_IsTrue(stop);
    cJSON_Delete(root);

    if (encerrar) {
        bt_audio_stop_discoverable_temporary();
        duration_s = 0;
    } else {
        bt_audio_enable_discoverable_temporary(duration_s);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "duration_s", duration_s);
    cJSON_AddNumberToObject(resp, "remaining_s", bt_audio_get_discoverable_remaining_s());
    return send_json(req, resp);
}

/* Desliga o Wi-Fi por um tempo limitado -- diagnostico pra isolar se um
 * ruido/interferencia no audio vem do radio Wi-Fi ou do Bluetooth (ver
 * wifi_manager_disable_temporarily()). O dispositivo reinicia sozinho no
 * fim da janela pra religar tudo -- responder ANTES de desligar, senao a
 * resposta HTTP nunca sai (o proprio Wi-Fi que levaria ela ja foi). */
static esp_err_t api_wifi_disable_temp_post(httpd_req_t *req)
{
    char buf[64];
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

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "duration_s", duration_s);
    esp_err_t err = send_json(req, resp);

    vTaskDelay(pdMS_TO_TICKS(200)); /* da tempo da resposta sair antes do Wi-Fi cair */
    wifi_manager_disable_temporarily(duration_s);
    return err;
}

/* Diagnostico manual -- pedido do usuario 2026-08-27: forca o reenvio de um
 * NOTIFY (GENA) com o estado atual do DLNA, sem mudar nada, pra testar se o
 * Music Assistant realmente reage a um evento novo (ver dlna_renderer_
 * force_notify()). "sent=false" quer dizer que nao ha assinatura ativa
 * agora -- nesse caso nao existe pra quem mandar, o teste fica inconclusivo
 * (nao e uma falha do NOTIFY em si). Conferir /api/logs pra ver se o envio
 * (quando sent=true) recebeu 2xx do lado do Music Assistant. */
/* Seletor de fonte de audio -- equivalente pela web ao botao fisico KEY1
 * (ver audio_source.h). Corpo: {"source": "bluetooth"|"dlna"} ou
 * {"toggle": true}. */
/* Diagnostico do microfone: escreve ADCCONTROL2 (selecao de entrada do ADC)
 * e/ou o ganho do PGA ao vivo, pra descobrir a configuracao certa desta
 * placa sem recompilar. Corpo: {"input": 0-255, "pga": 0-100}. */
/* GET /api/mic/raw?n=512 -- amostras cruas do ADC, uma por linha.
 * Diagnostico: permite analisar a FORMA do sinal (media, desvio, cruzamentos
 * por zero, saturacao) em vez de so o pico, que esconde tudo. */
/* true = /api/mic/raw devolve o sinal DEPOIS do processamento. */
static bool s_raw_da_saida = false;

static esp_err_t api_mic_raw_get(httpd_req_t *req)
{
    /* ?ch=r captura o canal DIREITO do conversor (padrao: esquerdo, que e o
     * que o caminho de audio usa). Existe porque o adaptador do microfone
     * pode por o sinal no anel do jack, e ai o sinal chega no direito. */
    {
        char q[32];
        char ch[4] = "";
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            httpd_query_key_value(q, "ch", ch, sizeof(ch));
        }
        audio_codec_mic_raw_set_canal(ch[0] == 'r' || ch[0] == 'R');
        char st[8] = "";
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            httpd_query_key_value(q, "stage", st, sizeof(st));
        }
        s_raw_da_saida = (st[0] == 'o' || st[0] == 'O');
    }

    size_t n = 512;
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[12];
        if (httpd_query_key_value(query, "n", val, sizeof(val)) == ESP_OK) {
            int q = atoi(val);
            if (q > 0 && q <= 2048) {
                n = (size_t)q;
            }
        }
    }
    int16_t *buf = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
        return ESP_FAIL;
    }
    size_t lidas = (s_raw_da_saida ? audio_codec_mic_capture_saida : audio_codec_mic_capture_raw)(buf, n);

    httpd_resp_set_type(req, "text/plain");
    char linha[16];
    for (size_t i = 0; i < lidas; i++) {
        int len = snprintf(linha, sizeof(linha), "%d\n", (int)buf[i]);
        httpd_resp_send_chunk(req, linha, len);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(buf);
    return ESP_OK;
}

/* POST /api/mic/hardreset -- corta o MCLK por 500ms e reconstroi I2S e codec.
 * Ver audio_codec_mic_hard_reset(). */
static esp_err_t api_mic_hardreset_post(httpd_req_t *req)
{
    esp_err_t err = audio_codec_mic_hard_reset();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "resultado", esp_err_to_name(err));
    cJSON_AddStringToObject(resp, "mic_adc", audio_codec_get_mic_adc_estado());
    return send_json(req, resp);
}

/* POST /api/amp {"on":true|false} -- força o estado do relé do amplificador.
 *
 * Ferramenta de INSTALAÇÃO: sem ela, testar a ligação do relé exige áudio
 * tocando e esperar o timeout, o que é péssimo com as mãos dentro da caixa.
 * Ligar aqui cancela o timer; qualquer áudio novo volta a mandar normalmente.
 *
 * Isto existiu antes como diagnóstico e foi removido na limpeza de 2026-08-30,
 * por eu julgar que o problema do relé estava resolvido. No mesmo dia o Célio
 * foi instalar o relé de verdade e precisou exatamente disto. Fica. */
static esp_err_t api_amp_post(httpd_req_t *req)
{
    char buf[48];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }
    cJSON *on = cJSON_GetObjectItem(root, "on");
    bool ligar = cJSON_IsTrue(on);
    cJSON_Delete(root);

    if (ligar) {
        relay_control_force_on();
    } else {
        relay_control_force_off();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "amplifier", relay_control_is_on());
    cJSON_AddBoolToObject(resp, "relay_active_low", relay_control_get_active_low());
    cJSON_AddBoolToObject(resp, "relay_open_drain", relay_control_get_open_drain());
    return send_json(req, resp);
}

/* POST /api/led/test {"gpio":N} -- pisca um GPIO por ~6s.
 *
 * Nasceu para descobrir onde ficavam os LEDs da placa (as referencias de
 * terceiros divergiam: GPIO19 aparecia como LED D5 numa e como KEY3/botao
 * noutra). Em vez de escolher no palpite, pisca e alguem olha -- foi assim que
 * o D5 foi confirmado em 2026-08-30.
 *
 * FICA como ferramenta: o plano e trocar os LEDs onboard por externos, ja que
 * dentro de uma caixa os da placa nao servem para nada, e este endpoint e o
 * jeito de conferir a ligacao de cada LED novo sem recompilar. Recusa o GPIO
 * do rele e o PA_ENABLE, que nao sao LED e teriam efeito real. */
static esp_err_t api_led_test_post(httpd_req_t *req)
{
    char buf[48];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }
    cJSON *g = cJSON_GetObjectItem(root, "gpio");
    int gpio = cJSON_IsNumber(g) ? g->valueint : -1;
    cJSON_Delete(root);
    if (gpio < 0 || gpio > 39) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "informe \"gpio\"");
        return ESP_FAIL;
    }
    /* Nunca mexer no rele/LED verde por aqui: ligaria o amplificador. */
    if (gpio == PIN_RELAY_CONTROL || gpio == PIN_PA_ENABLE_DO_NOT_USE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "GPIO reservado");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "gpio", gpio);
    esp_err_t err = send_json(req, resp);

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    for (int i = 0; i < 12; i++) {
        gpio_set_level((gpio_num_t)gpio, i % 2);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    /* Devolve o pino ao estado neutro -- se for botao, volta a ser entrada. */
    gpio_reset_pin((gpio_num_t)gpio);
    return err;
}

/* Diagnostico: le/escreve registrador do ES8388 ao vivo.
 * {"reg": N}            -> le e devolve o valor
 * {"reg": N, "val": V}  -> escreve V e devolve o valor lido de volta */
static esp_err_t api_mic_reg_post(httpd_req_t *req)
{
    char buf[64];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }
    cJSON *reg = cJSON_GetObjectItem(root, "reg");
    cJSON *val = cJSON_GetObjectItem(root, "val");
    if (!cJSON_IsNumber(reg)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "informe \"reg\"");
        return ESP_FAIL;
    }
    uint8_t r = (uint8_t)reg->valueint;
    if (cJSON_IsNumber(val)) {
        es8388_write_reg_raw(r, (uint8_t)val->valueint);
    }
    cJSON_Delete(root);

    uint8_t lido = 0;
    esp_err_t err = es8388_read_reg_raw(r, &lido);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(resp, "reg", r);
    cJSON_AddNumberToObject(resp, "value", lido);
    return send_json(req, resp);
}

static esp_err_t api_source_post(httpd_req_t *req)
{
    char buf[64];
    cJSON *root = recv_json_body(req, buf, sizeof(buf));
    if (root == NULL) {
        return ESP_FAIL;
    }
    cJSON *toggle = cJSON_GetObjectItem(root, "toggle");
    cJSON *src = cJSON_GetObjectItem(root, "source");
    bool invalido = false;
    if (cJSON_IsTrue(toggle)) {
        audio_source_toggle();
    } else if (cJSON_IsString(src) && src->valuestring != NULL) {
        if (strcmp(src->valuestring, "bluetooth") == 0) {
            audio_source_set(AUDIO_SOURCE_BT);
        } else if (strcmp(src->valuestring, "dlna") == 0) {
            audio_source_set(AUDIO_SOURCE_DLNA);
        } else {
            invalido = true;
        }
    } else {
        invalido = true;
    }
    cJSON_Delete(root);

    if (invalido) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "source deve ser \"bluetooth\" ou \"dlna\" (ou toggle: true)");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "source", audio_source_name(audio_source_get()));
    return send_json(req, resp);
}

static esp_err_t api_dlna_force_notify_post(httpd_req_t *req)
{
    bool sent = dlna_renderer_force_notify();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "sent", sent);
    if (!sent) {
        cJSON_AddStringToObject(resp, "message", "sem assinatura DLNA ativa agora -- nada pra notificar");
    }
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
        /* Bloquear so impedia reconexoes FUTURAS -- um dispositivo ja
         * conectado no momento do clique continuava tocando normalmente
         * ate desconectar sozinho (confirmado pelo usuario testando).
         * Derruba agora se for o caso. */
        bt_audio_status_t bt;
        bt_audio_get_status(&bt);
        uint8_t connected_mac[6];
        if (bt.connected && pairing_parse_mac(bt.remote_mac, connected_mac) &&
            memcmp(connected_mac, mac, 6) == 0) {
            bt_audio_disconnect_device(mac);
        }
    } else if (strcmp(action, "remove") == 0) {
        /* pairing_clear_device() (nao so pairing_remove_from_history()):
         * "remover" tambem deve zerar autorizado/bloqueado do mac, senao um
         * bloqueio antigo sobrevive escondido -- reaparece do nada na
         * proxima vez que esse mac tentar conectar, sem estar visivel em
         * lugar nenhum da interface (mesma causa raiz do bug em
         * pairing_clear_device, ver pairing.c). */
        pairing_clear_device(mac);
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
    /* 21 rotas hoje. Ja estourou uma vez (2026-08-28): o limite estava em 20
     * e a rota curinga (a que serve as paginas do SPIFFS) e registrada
     * POR ULTIMO, entao foi justamente ela que falhou, e a interface inteira
     * passou a responder 404 ("Nothing matches the given URI") enquanto a API
     * continuava funcionando. Sintoma confuso pra um limite silencioso.
     * Folga generosa aqui e barata (cada slot e so um ponteiro). */
    config.max_uri_handlers = 32;
    config.stack_size = 8192; /* /ota escreve na flash — folga extra de pilha */
    config.recv_wait_timeout = 10;
    /* RECICLA A CONEXAO MAIS ANTIGA em vez de RECUSAR a nova (2026-09-19).
     *
     * Sem isto, quando os sockets acabam o servidor simplesmente nega conexoes
     * novas -- e o navegador, que abre varias em paralelo (HTML, CSS e o
     * polling de status), encontra a porta fechada. Da interface travada,
     * enquanto uma requisicao solitaria por curl continua respondendo
     * normalmente. Foi exatamente esse o sintoma relatado pelo Celio com o
     * Bluetooth conectado: `/api/status` respondendo 25 de 25 pelo terminal e
     * a pagina parada no navegador.
     *
     * A causa de fundo e a RAM interna, que com Bluetooth conectado E
     * microfone ligado cai para ~10KB (ja esteve em 25,5KB) -- o Bluetooth
     * sozinho consome ~11KB, e isso e problema conhecido deste projeto. Menos
     * sockets, cada um com o seu buffer, e purga do mais antigo em vez de
     * recusa: a pagina passa a carregar mesmo no aperto. */
    config.lru_purge_enable = true;
    /* Menos sockets simultaneos, porem sempre atendidos. O default (7) e
     * generoso para um servidor que atende uma pagina por vez, e cada socket
     * custa buffer de RAM interna -- justamente o que falta. */
    config.max_open_sockets = 4;

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
        {.uri = "/api/wifi/disable_temp", .method = HTTP_POST, .handler = api_wifi_disable_temp_post},
        {.uri = "/api/dlna/force_notify", .method = HTTP_POST, .handler = api_dlna_force_notify_post},
        {.uri = "/api/source", .method = HTTP_POST, .handler = api_source_post},
        {.uri = "/api/mic/raw", .method = HTTP_GET, .handler = api_mic_raw_get},
        {.uri = "/api/mic/reg", .method = HTTP_POST, .handler = api_mic_reg_post},
        {.uri = "/api/led/test", .method = HTTP_POST, .handler = api_led_test_post},
        {.uri = "/api/amp", .method = HTTP_POST, .handler = api_amp_post},
        {.uri = "/api/mic/hardreset", .method = HTTP_POST, .handler = api_mic_hardreset_post},
        {.uri = "/api/system/restart", .method = HTTP_POST, .handler = api_system_restart_post},
        {.uri = "/api/system/beep", .method = HTTP_POST, .handler = api_system_beep_post},
        /* GET tambem, de proposito -- diagnostico pra acionar direto da
         * barra de enderecos do navegador (util remotamente, sem precisar
         * de curl/Postman a mao). */
        {.uri = "/api/system/beep", .method = HTTP_GET, .handler = api_system_beep_post},
        {.uri = "/ota", .method = HTTP_POST, .handler = ota_manager_upload_handler},
        {.uri = "/ota/spiffs", .method = HTTP_POST, .handler = ota_manager_spiffs_upload_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = static_file_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    logger_log(ESP_LOG_INFO, TAG, "Servidor web iniciado na porta %d", config.server_port);
}
