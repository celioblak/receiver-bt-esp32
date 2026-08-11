#include "mqtt_ha.h"

#include <stdlib.h>
#include <string.h>

#include "audio_agc.h"
#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "logger.h"
#include "relay_control.h"
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

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;

static void publish_sensor_discovery(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Receiver Bluetooth DIY");
    cJSON_AddStringToObject(root, "state_topic", TOPIC_STATE);
    cJSON_AddStringToObject(root, "unique_id", "receiver_bt_status");
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.playing_status }}");
    cJSON_AddStringToObject(root, "json_attributes_topic", TOPIC_STATE);

    cJSON *device = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString("receiver_bt"));
    cJSON_AddItemToObject(device, "identifiers", ids);
    cJSON_AddStringToObject(device, "name", "Receiver Bluetooth DIY");
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

    cJSON *device = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString("receiver_bt"));
    cJSON_AddItemToObject(device, "identifiers", ids);
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

static void publish_all_discovery_configs(void)
{
    publish_sensor_discovery();

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
    char payload[16];
    copy_payload(event, payload, sizeof(payload));

    if (topic_is(event, TOPIC_CMD_MEDIA)) {
        bt_audio_media_control(payload);
    } else if (topic_is(event, TOPIC_CMD_VOLUME)) {
        audio_codec_set_volume(atoi(payload));
    } else if (topic_is(event, TOPIC_CMD_AGC_ENABLED)) {
        audio_agc_enable(strcmp(payload, "1") == 0);
    } else if (topic_is(event, TOPIC_CMD_DISCOVERABLE)) {
        bt_audio_set_discoverable(strcmp(payload, "1") == 0);
    } else if (topic_is(event, TOPIC_CMD_REQUIRE_PIN)) {
        bt_audio_set_require_pin(strcmp(payload, "1") == 0);
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

    char device_name[32];
    if (storage_get_str(NVS_KEY_DEVICE_NAME, device_name, sizeof(device_name)) != ESP_OK) {
        strlcpy(device_name, FW_DEVICE_NAME_DEFAULT, sizeof(device_name));
    }

    char ip[16];
    wifi_manager_get_ip_str(ip, sizeof(ip));

    const char *status_str = bt.connected ? (bt.playing ? "playing" : "connected") : "disconnected";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "playing_status", status_str);
    cJSON_AddBoolToObject(root, "connected", bt.connected);
    cJSON_AddStringToObject(root, "device", device_name);
    cJSON_AddStringToObject(root, "track", bt.title);
    cJSON_AddStringToObject(root, "artist", bt.artist);
    cJSON_AddStringToObject(root, "album", bt.album);
    cJSON_AddNumberToObject(root, "volume", audio_codec_get_volume());
    cJSON_AddBoolToObject(root, "amplifier", relay_control_is_on());
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddBoolToObject(root, "agc_enabled", audio_agc_is_enabled());
    cJSON_AddNumberToObject(root, "agc_target", audio_agc_get_target());
    cJSON_AddNumberToObject(root, "agc_mode", audio_agc_get_mode());
    cJSON_AddBoolToObject(root, "bt_discoverable", bt_audio_get_discoverable());
    cJSON_AddBoolToObject(root, "bt_require_pin", bt_audio_get_require_pin());
    if (bt.pending_pin_code[0] != '\0') {
        cJSON_AddStringToObject(root, "pending_pin_mac", bt.pending_pin_mac);
        cJSON_AddStringToObject(root, "pending_pin_code", bt.pending_pin_code);
    }

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, TOPIC_STATE, payload, 0, 0, false);
    free(payload);
    cJSON_Delete(root);
}
