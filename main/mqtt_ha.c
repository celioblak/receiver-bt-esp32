#include "mqtt_ha.h"

#include <string.h>

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

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;

static void publish_discovery_config(void)
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

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            logger_log(ESP_LOG_INFO, TAG, "MQTT conectado");
            publish_discovery_config();
            mqtt_ha_publish_state();
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            logger_log(ESP_LOG_WARN, TAG, "MQTT desconectado");
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

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client, TOPIC_STATE, payload, 0, 0, false);
    free(payload);
    cJSON_Delete(root);
}
