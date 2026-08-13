#include "mqtt_ha.h"

#include <stdlib.h>
#include <string.h>

#include "audio_agc.h"
#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "lms_cli.h"
#include "lms_metadata.h"
#include "logger.h"
#include "pairing.h"
#include "relay_control.h"
#include "slimproto.h"
#include "storage.h"
#include "wifi_manager.h"

#include "cJSON.h"
#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt_ha";

#define TOPIC_STATE  "homeassistant/sensor/receiver_bt/state"
#define TOPIC_CONFIG "homeassistant/sensor/receiver_bt/config"

/* Comandos: topicos proprios (nao um schema oficial de "media_player" da
 * HA, que nao cobre bem next/previous) -- documentados no README, dá pra
 * usar via automacoes/scripts da Home Assistant ou MQTT Explorer direto. */
#define TOPIC_CMD_MEDIA         "homeassistant/receiver_bt/cmd/media"
#define TOPIC_CMD_VOLUME        "homeassistant/receiver_bt/cmd/volume"
#define TOPIC_CMD_AGC_ENABLED   "homeassistant/receiver_bt/cmd/agc_enabled"
#define TOPIC_CMD_DISCOVERABLE  "homeassistant/receiver_bt/cmd/bt_discoverable"
#define TOPIC_CMD_REQUIRE_PIN   "homeassistant/receiver_bt/cmd/bt_require_pin"
#define TOPIC_CMD_DISCONNECT    "homeassistant/receiver_bt/cmd/disconnect"
#define TOPIC_CMD_RELAY_TIMEOUT "homeassistant/receiver_bt/cmd/relay_timeout"
#define TOPIC_CMD_PAIR          "homeassistant/receiver_bt/cmd/pair"

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;

/* O nome do dispositivo na Home Assistant ficava travado no literal
 * "Receiver Bluetooth DIY", ignorando o nome configurado via
 * /api/config (device_name) -- confirmado pelo usuario, o dispositivo MQTT
 * criado na HA nao refletia o nome real. Le da NVS toda vez (nao troca com
 * frequencia, custo de flash irrelevante aqui). */
static void get_configured_device_name(char *out, size_t out_len)
{
    if (storage_get_str(NVS_KEY_DEVICE_NAME, out, out_len) != ESP_OK || out[0] == '\0') {
        strlcpy(out, FW_DEVICE_NAME_DEFAULT, out_len);
    }
}

static void publish_sensor_discovery(void)
{
    char device_name[32];
    get_configured_device_name(device_name, sizeof(device_name));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", device_name);
    cJSON_AddStringToObject(root, "state_topic", TOPIC_STATE);
    cJSON_AddStringToObject(root, "unique_id", "receiver_bt_status");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.playing_status }}");
    cJSON_AddStringToObject(root, "json_attributes_topic", TOPIC_STATE);

    cJSON *device = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString("receiver_bt"));
    cJSON_AddItemToObject(device, "identifiers", ids);
    cJSON_AddStringToObject(device, "name", device_name);
    cJSON_AddStringToObject(device, "manufacturer", "DIY");
    cJSON_AddStringToObject(device, "model", "ESP32 Audio Kit V2.2");
    cJSON_AddItemToObject(root, "device", device);

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, TOPIC_CONFIG, payload, 0, 1, true);
    free(payload);
    cJSON_Delete(root);
}

/* Helper comum pra discovery: todos os entity types da HA usam esses
 * mesmos 3 campos (unique_id/device/state_topic com json_attributes ja
 * cobre diagnostico -- so os campos especificos de cada tipo variam). */
static void add_common_device_fields(cJSON *root, const char *unique_id, const char *name)
{
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "availability_topic", TOPIC_STATE);
    cJSON_AddStringToObject(root, "availability_template", "{{ 'online' }}");

    char device_name[32];
    get_configured_device_name(device_name, sizeof(device_name));

    cJSON *device = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString("receiver_bt"));
    cJSON_AddItemToObject(device, "identifiers", ids);
    cJSON_AddStringToObject(device, "name", device_name);
    cJSON_AddItemToObject(root, "device", device);
}

static void publish_switch_discovery(const char *object_id, const char *name, const char *cmd_topic,
                                      const char *value_template)
{
    char config_topic[80];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/switch/%s/config", object_id);

    cJSON *root = cJSON_CreateObject();
    add_common_device_fields(root, object_id, name);
    cJSON_AddStringToObject(root, "command_topic", cmd_topic);
    cJSON_AddStringToObject(root, "state_topic", TOPIC_STATE);
    cJSON_AddStringToObject(root, "value_template", value_template);
    cJSON_AddStringToObject(root, "payload_on", "1");
    cJSON_AddStringToObject(root, "payload_off", "0");
    cJSON_AddStringToObject(root, "state_on", "1");
    cJSON_AddStringToObject(root, "state_off", "0");

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, true);
    free(payload);
    cJSON_Delete(root);
}

static void publish_number_discovery(const char *object_id, const char *name, const char *cmd_topic,
                                      const char *value_template, int min, int max)
{
    char config_topic[80];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/number/%s/config", object_id);

    cJSON *root = cJSON_CreateObject();
    add_common_device_fields(root, object_id, name);
    cJSON_AddStringToObject(root, "command_topic", cmd_topic);
    cJSON_AddStringToObject(root, "state_topic", TOPIC_STATE);
    cJSON_AddStringToObject(root, "value_template", value_template);
    cJSON_AddNumberToObject(root, "min", min);
    cJSON_AddNumberToObject(root, "max", max);
    cJSON_AddNumberToObject(root, "step", 1);

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, true);
    free(payload);
    cJSON_Delete(root);
}

static void publish_button_discovery(const char *object_id, const char *name, const char *cmd_topic,
                                      const char *payload_press)
{
    char config_topic[80];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/button/%s/config", object_id);

    cJSON *root = cJSON_CreateObject();
    add_common_device_fields(root, object_id, name);
    cJSON_AddStringToObject(root, "command_topic", cmd_topic);
    cJSON_AddStringToObject(root, "payload_press", payload_press);

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, true);
    free(payload);
    cJSON_Delete(root);
}

static void publish_binary_sensor_discovery(const char *object_id, const char *name,
                                             const char *device_class, const char *value_template)
{
    char config_topic[80];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/binary_sensor/%s/config", object_id);

    cJSON *root = cJSON_CreateObject();
    add_common_device_fields(root, object_id, name);
    cJSON_AddStringToObject(root, "device_class", device_class);
    cJSON_AddStringToObject(root, "state_topic", TOPIC_STATE);
    cJSON_AddStringToObject(root, "value_template", value_template);
    cJSON_AddStringToObject(root, "payload_on", "1");
    cJSON_AddStringToObject(root, "payload_off", "0");

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, true);
    free(payload);
    cJSON_Delete(root);
}

static void publish_generic_sensor_discovery(const char *object_id, const char *name,
                                              const char *value_template)
{
    char config_topic[80];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", object_id);

    cJSON *root = cJSON_CreateObject();
    add_common_device_fields(root, object_id, name);
    cJSON_AddStringToObject(root, "state_topic", TOPIC_STATE);
    cJSON_AddStringToObject(root, "value_template", value_template);
    cJSON_AddStringToObject(root, "json_attributes_topic", TOPIC_STATE);

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, true);
    free(payload);
    cJSON_Delete(root);
}

/* Converte "aa:bb:cc:dd:ee:ff" -> "aabbccddeeff" (sem separador), usado pra
 * montar um object_id/unique_id valido pra HA (nao aceita ':') a partir do
 * MAC de cada dispositivo pareado. */
static void mac_to_object_id_suffix(const char *mac_str, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; mac_str[i] != '\0' && j + 1 < out_len; i++) {
        if (mac_str[i] != ':') {
            out[j++] = mac_str[i];
        }
    }
    out[j] = '\0';
}

/* Um switch por dispositivo ja pareado (liga=autorizado, desliga=bloqueado)
 * -- pedido explicito do usuario, pra bloquear/autorizar sem precisar abrir
 * a interface web. Republicado toda vez que mqtt_ha_publish_state() roda
 * (nao so uma vez na conexao), porque novos dispositivos podem parear a
 * qualquer momento -- discovery e retido, entao republicar o mesmo
 * unique_id e barato/idempotente pro broker/HA. Estado e comando
 * compartilham topicos unicos (TOPIC_STATE/TOPIC_CMD_PAIR); o mac de cada
 * dispositivo fica embutido no proprio template (value_template usa
 * selectattr pra achar a entrada certa no array "paired_devices"). */
static void publish_pair_switch_discovery(const char *mac_str, const char *name)
{
    char suffix[13];
    mac_to_object_id_suffix(mac_str, suffix, sizeof(suffix));

    char object_id[40];
    snprintf(object_id, sizeof(object_id), "receiver_bt_pair_%s", suffix);
    char config_topic[80];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/switch/%s/config", object_id);

    char value_template[220];
    snprintf(value_template, sizeof(value_template),
             "{{ '1' if (value_json.paired_devices | selectattr('mac','equalto','%s') | list | first).allowed else '0' }}",
             mac_str);
    char command_template[96];
    snprintf(command_template, sizeof(command_template),
             "{\"mac\":\"%s\",\"action\":\"{{ 'allow' if value == '1' else 'block' }}\"}", mac_str);

    cJSON *root = cJSON_CreateObject();
    add_common_device_fields(root, object_id, name);
    cJSON_AddStringToObject(root, "command_topic", TOPIC_CMD_PAIR);
    cJSON_AddStringToObject(root, "command_template", command_template);
    cJSON_AddStringToObject(root, "state_topic", TOPIC_STATE);
    cJSON_AddStringToObject(root, "value_template", value_template);
    cJSON_AddStringToObject(root, "payload_on", "1");
    cJSON_AddStringToObject(root, "payload_off", "0");
    cJSON_AddStringToObject(root, "state_on", "1");
    cJSON_AddStringToObject(root, "state_off", "0");

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, true);
    free(payload);
    cJSON_Delete(root);
}

static void publish_all_discovery_configs(void)
{
    publish_sensor_discovery();

    /* Pedido explicito do usuario: o "dispositivo conectado" e a lista de
     * "dispositivos pareados" so apareciam como atributos json enterrados
     * no sensor de diagnostico -- viram entidades proprias aqui, mais
     * visiveis/usaveis em automacoes e no card do dispositivo na HA. */
    publish_generic_sensor_discovery("receiver_bt_connected_device", "Receiver BT Dispositivo Conectado",
                                      "{{ value_json.connected_device }}");
    publish_generic_sensor_discovery("receiver_bt_paired_count", "Receiver BT Dispositivos Pareados",
                                      "{{ value_json.paired_count }}");
    publish_generic_sensor_discovery("receiver_bt_ma_host", "Receiver BT Servidor Music Assistant",
                                      "{{ value_json.ma_host }}");
    publish_button_discovery("receiver_bt_disconnect", "Receiver BT Desconectar", TOPIC_CMD_DISCONNECT, "disconnect");
    /* Os switches de cada dispositivo pareado (publish_pair_switch_discovery)
     * NAO sao publicados aqui -- ver mqtt_ha_publish_state(), que roda logo
     * em seguida e a cada 30s dali pra frente, cobrindo tanto o boot quanto
     * dispositivos pareados depois. */

    /* "Configuracoes": timeout do rele e o unico ajuste de config que faz
     * sentido expor como entidade MQTT com seguranca -- WiFi/MQTT
     * reconfigurados PELO PROPRIO MQTT seria circular (se o host/senha
     * mudar errado, o dispositivo perde a conexao MQTT e fica sem como
     * corrigir por ali). Esses continuam so na interface web/API REST. */
    publish_number_discovery("receiver_bt_relay_timeout", "Receiver BT Timeout do Amplificador",
                              TOPIC_CMD_RELAY_TIMEOUT, "{{ value_json.relay_timeout_s }}", 5, 600);

    /* "problem" fica visivel na HA como badge de alerta no card do
     * dispositivo -- token JWT do Music Assistant expira (normalmente 1
     * ano) e, sem isso, titulo/artista/album somem em silencio sem
     * nenhum aviso obvio de por que (ver lms_metadata.h). So acende se um
     * token FOI configurado e foi rejeitado -- "nao configurado" nao e
     * problema, e so um recurso opcional desligado. */
    publish_binary_sensor_discovery("receiver_bt_ma_token", "Receiver BT Token Music Assistant",
                                     "problem", "{{ '1' if value_json.ma_token_problem else '0' }}");

    publish_number_discovery("receiver_bt_volume", "Receiver BT Volume", TOPIC_CMD_VOLUME,
                              "{{ value_json.volume }}", 0, VOLUME_STEPS);
    publish_switch_discovery("receiver_bt_agc", "Receiver BT AGC", TOPIC_CMD_AGC_ENABLED,
                              "{{ '1' if value_json.agc_enabled else '0' }}");
    publish_switch_discovery("receiver_bt_discoverable", "Receiver BT Visivel", TOPIC_CMD_DISCOVERABLE,
                              "{{ '1' if value_json.bt_discoverable else '0' }}");
    publish_switch_discovery("receiver_bt_require_pin", "Receiver BT Exigir PIN", TOPIC_CMD_REQUIRE_PIN,
                              "{{ '1' if value_json.bt_require_pin else '0' }}");

    publish_button_discovery("receiver_bt_play", "Receiver BT Play", TOPIC_CMD_MEDIA, "play");
    publish_button_discovery("receiver_bt_pause", "Receiver BT Pause", TOPIC_CMD_MEDIA, "pause");
    publish_button_discovery("receiver_bt_next", "Receiver BT Proxima", TOPIC_CMD_MEDIA, "next");
    publish_button_discovery("receiver_bt_previous", "Receiver BT Anterior", TOPIC_CMD_MEDIA, "previous");
}

static void subscribe_commands(void)
{
    esp_mqtt_client_subscribe(s_client, TOPIC_CMD_MEDIA, 0);
    esp_mqtt_client_subscribe(s_client, TOPIC_CMD_VOLUME, 0);
    esp_mqtt_client_subscribe(s_client, TOPIC_CMD_AGC_ENABLED, 0);
    esp_mqtt_client_subscribe(s_client, TOPIC_CMD_DISCOVERABLE, 0);
    esp_mqtt_client_subscribe(s_client, TOPIC_CMD_REQUIRE_PIN, 0);
    esp_mqtt_client_subscribe(s_client, TOPIC_CMD_DISCONNECT, 0);
    esp_mqtt_client_subscribe(s_client, TOPIC_CMD_RELAY_TIMEOUT, 0);
    esp_mqtt_client_subscribe(s_client, TOPIC_CMD_PAIR, 0);
}

/* payload de MQTT_EVENT_DATA nao vem terminado em '\0' -- copia pra um
 * buffer local antes de comparar/converter. */
static void copy_payload(const esp_mqtt_event_handle_t event, char *out, size_t out_size)
{
    size_t len = (size_t)event->data_len < out_size - 1 ? (size_t)event->data_len : out_size - 1;
    memcpy(out, event->data, len);
    out[len] = '\0';
}

static bool topic_is(const esp_mqtt_event_handle_t event, const char *topic)
{
    size_t len = strlen(topic);
    return (size_t)event->topic_len == len && memcmp(event->topic, topic, len) == 0;
}

static void handle_command(esp_mqtt_event_handle_t event)
{
    /* 128 (nao 16): TOPIC_CMD_PAIR manda um JSON pequeno
     * ({"mac":"aa:bb:cc:dd:ee:ff","action":"allow"}, ~45 bytes) -- os
     * outros comandos continuam curtos, sobra folga de qualquer forma. */
    char payload[128];
    copy_payload(event, payload, sizeof(payload));

    if (topic_is(event, TOPIC_CMD_MEDIA)) {
        /* Mesma regra do POST /api/media (web_server.c): BT tem
         * prioridade, senao tenta Slimproto/Music Assistant -- antes disso
         * os botoes de midia da Home Assistant so funcionavam via AVRCP,
         * nunca controlavam o Music Assistant. */
        bt_audio_status_t bt;
        bt_audio_get_status(&bt);
        if (bt.connected) {
            bt_audio_media_control(payload);
        } else {
            slimproto_status_t slim;
            slimproto_get_status(&slim);
            if (slim.connected) {
                lms_cli_send_transport(payload);
            }
        }
    } else if (topic_is(event, TOPIC_CMD_VOLUME)) {
        audio_codec_set_volume(atoi(payload));
    } else if (topic_is(event, TOPIC_CMD_AGC_ENABLED)) {
        audio_agc_enable(strcmp(payload, "1") == 0);
    } else if (topic_is(event, TOPIC_CMD_DISCOVERABLE)) {
        bt_audio_set_discoverable(strcmp(payload, "1") == 0);
    } else if (topic_is(event, TOPIC_CMD_REQUIRE_PIN)) {
        bt_audio_set_require_pin(strcmp(payload, "1") == 0);
    } else if (topic_is(event, TOPIC_CMD_DISCONNECT)) {
        bt_audio_status_t bt;
        bt_audio_get_status(&bt);
        uint8_t mac[6];
        if (bt.connected && pairing_parse_mac(bt.remote_mac, mac)) {
            bt_audio_disconnect_device(mac);
        }
    } else if (topic_is(event, TOPIC_CMD_RELAY_TIMEOUT)) {
        storage_set_i32(NVS_KEY_RELAY_TIMEOUT, atoi(payload));
    } else if (topic_is(event, TOPIC_CMD_PAIR)) {
        cJSON *cmd = cJSON_Parse(payload);
        if (cmd != NULL) {
            cJSON *mac_item = cJSON_GetObjectItem(cmd, "mac");
            cJSON *action_item = cJSON_GetObjectItem(cmd, "action");
            uint8_t mac[6];
            if (cJSON_IsString(mac_item) && cJSON_IsString(action_item) &&
                pairing_parse_mac(mac_item->valuestring, mac)) {
                pairing_set_allowed(mac, strcmp(action_item->valuestring, "allow") == 0);
            }
            cJSON_Delete(cmd);
        }
    } else {
        return;
    }
    mqtt_ha_publish_state();
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            logger_log(ESP_LOG_INFO, TAG, "MQTT conectado");
            publish_all_discovery_configs();
            subscribe_commands();
            mqtt_ha_publish_state();
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            logger_log(ESP_LOG_WARN, TAG, "MQTT desconectado");
            break;
        case MQTT_EVENT_DATA:
            handle_command(event);
            break;
        default:
            break;
    }
}

void mqtt_ha_init(void)
{
    char host[64];
    if (storage_get_str(NVS_KEY_MQTT_HOST, host, sizeof(host)) != ESP_OK || strlen(host) == 0) {
        logger_log(ESP_LOG_INFO, TAG, "MQTT nao configurado, integracao com Home Assistant desativada");
        return;
    }

    int32_t port = 1883;
    storage_get_i32(NVS_KEY_MQTT_PORT, &port, 1883);

    char user[32] = {0};
    char pass[64] = {0};
    storage_get_str(NVS_KEY_MQTT_USER, user, sizeof(user));
    storage_get_str(NVS_KEY_MQTT_PASS, pass, sizeof(pass));

    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", host, (int)port);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = user[0] ? user : NULL,
        .credentials.authentication.password = pass[0] ? pass : NULL,
    };

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    logger_log(ESP_LOG_INFO, TAG, "Conectando ao broker MQTT %s:%d", host, (int)port);
}

void mqtt_ha_publish_state(void)
{
    if (!s_connected || s_client == NULL) {
        return;
    }

    bt_audio_status_t bt;
    bt_audio_get_status(&bt);

    slimproto_status_t slim;
    slimproto_get_status(&slim);

    char device_name[32];
    get_configured_device_name(device_name, sizeof(device_name));

    char ip[16];
    wifi_manager_get_ip_str(ip, sizeof(ip));

    /* BT tem prioridade (mesma regra do audio em si) -- so usa
     * titulo/artista/album do Music Assistant (via lms_metadata, API
     * propria do MA) quando nao ha celular conectado. */
    const char *track = bt.title;
    const char *artist = bt.artist;
    const char *album = bt.album;
    bool connected = bt.connected;
    bool playing = bt.playing;
    lms_metadata_t lms_meta = {0};
    if (!bt.connected && slim.connected) {
        lms_metadata_get(&lms_meta);
        track = lms_meta.title;
        artist = lms_meta.artist;
        album = lms_meta.album;
        connected = true;
        playing = lms_meta.valid ? lms_meta.playing : slim.playing;
    }
    const char *status_str = connected ? (playing ? "playing" : "connected") : "disconnected";

    bool ma_configured = lms_metadata_is_configured();
    bool ma_token_problem = ma_configured && lms_metadata_auth_failed();

    /* "Dispositivo conectado" e "dispositivos pareados" pedidos como
     * entidades proprias (ver publish_all_discovery_configs) -- calculados
     * aqui, no mesmo payload de estado que elas leem via value_template. */
    char connected_device[64] = "Nenhum";
    if (bt.connected) {
        pairing_device_t history[PAIRING_HISTORY_MAX];
        size_t n = pairing_get_history(history, PAIRING_HISTORY_MAX);
        strlcpy(connected_device, bt.remote_mac, sizeof(connected_device));
        for (size_t i = 0; i < n; i++) {
            char mac_str[18];
            pairing_format_mac(history[i].mac, mac_str, sizeof(mac_str));
            if (strcmp(mac_str, bt.remote_mac) == 0 && history[i].name[0] != '\0') {
                strlcpy(connected_device, history[i].name, sizeof(connected_device));
                break;
            }
        }
    } else if (connected) {
        strlcpy(connected_device, "Music Assistant", sizeof(connected_device));
    }

    pairing_device_t paired_history[PAIRING_HISTORY_MAX];
    size_t paired_count = pairing_get_history(paired_history, PAIRING_HISTORY_MAX);
    char paired_names[PAIRING_HISTORY_MAX * 34] = "";
    for (size_t i = 0; i < paired_count; i++) {
        if (i > 0) {
            strlcat(paired_names, ", ", sizeof(paired_names));
        }
        strlcat(paired_names, paired_history[i].name[0] ? paired_history[i].name : "?", sizeof(paired_names));
    }

    int32_t relay_timeout = DEFAULT_RELAY_TIMEOUT_S;
    storage_get_i32(NVS_KEY_RELAY_TIMEOUT, &relay_timeout, DEFAULT_RELAY_TIMEOUT_S);

    char ma_host[64] = "";
    storage_get_str(NVS_KEY_SLIM_HOST, ma_host, sizeof(ma_host));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "playing_status", status_str);
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddStringToObject(root, "device", device_name);
    cJSON_AddStringToObject(root, "track", track);
    cJSON_AddStringToObject(root, "artist", artist);
    cJSON_AddStringToObject(root, "album", album);
    cJSON_AddBoolToObject(root, "ma_configured", ma_configured);
    cJSON_AddBoolToObject(root, "ma_token_problem", ma_token_problem);
    cJSON_AddNumberToObject(root, "volume", audio_codec_get_volume());
    cJSON_AddBoolToObject(root, "amplifier", relay_control_is_on());
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddBoolToObject(root, "agc_enabled", audio_agc_is_enabled());
    cJSON_AddNumberToObject(root, "agc_target", audio_agc_get_target());
    cJSON_AddNumberToObject(root, "agc_mode", audio_agc_get_mode());
    cJSON_AddBoolToObject(root, "bt_discoverable", bt_audio_get_discoverable());
    cJSON_AddBoolToObject(root, "bt_require_pin", bt_audio_get_require_pin());
    cJSON_AddStringToObject(root, "connected_device", connected_device);
    cJSON_AddNumberToObject(root, "paired_count", (double)paired_count);
    cJSON_AddStringToObject(root, "paired_names", paired_names);
    cJSON_AddNumberToObject(root, "relay_timeout_s", relay_timeout);
    cJSON_AddStringToObject(root, "ma_host", ma_host);
    if (bt.pending_pin_code[0] != '\0') {
        cJSON_AddStringToObject(root, "pending_pin_mac", bt.pending_pin_mac);
        cJSON_AddStringToObject(root, "pending_pin_code", bt.pending_pin_code);
    }

    /* "paired_devices": array com mac/nome/autorizado -- e o que os
     * switches por dispositivo (publish_pair_switch_discovery) leem via
     * value_template (selectattr por mac). Republicamos os proprios
     * switches logo abaixo, nao so uma vez na conexao, porque um dispositivo
     * pode parear a qualquer momento depois. */
    cJSON *paired_arr = cJSON_CreateArray();
    for (size_t i = 0; i < paired_count; i++) {
        char mac_str[18];
        pairing_format_mac(paired_history[i].mac, mac_str, sizeof(mac_str));
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "mac", mac_str);
        cJSON_AddStringToObject(entry, "name", paired_history[i].name[0] ? paired_history[i].name : mac_str);
        cJSON_AddBoolToObject(entry, "allowed", pairing_is_allowed(paired_history[i].mac));
        cJSON_AddItemToArray(paired_arr, entry);
    }
    cJSON_AddItemToObject(root, "paired_devices", paired_arr);

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, TOPIC_STATE, payload, 0, 0, false);
    free(payload);

    for (size_t i = 0; i < paired_count; i++) {
        char mac_str[18];
        pairing_format_mac(paired_history[i].mac, mac_str, sizeof(mac_str));
        publish_pair_switch_discovery(mac_str, paired_history[i].name[0] ? paired_history[i].name : mac_str);
    }
    cJSON_Delete(root);
}
