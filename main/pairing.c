#include "pairing.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "logger.h"
#include "storage.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "pairing";

typedef struct {
    uint8_t count;
    uint8_t macs[PAIRING_MAX_ALLOWED][6];
} allowed_list_t;

typedef struct {
    uint8_t count;
    pairing_device_t entries[PAIRING_HISTORY_MAX]; /* [0] = mais recente */
} history_list_t;

static allowed_list_t s_allowed;
static history_list_t s_history;
static SemaphoreHandle_t s_mutex;

static bool mac_eq(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static void save_allowed(void)
{
    storage_set_blob(NVS_KEY_PAIRED_MACS, &s_allowed, sizeof(s_allowed));
}

static void save_history(void)
{
    storage_set_blob(NVS_KEY_DEVICE_HISTORY, &s_history, sizeof(s_history));
}

void pairing_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    size_t len = sizeof(s_allowed);
    if (storage_get_blob(NVS_KEY_PAIRED_MACS, &s_allowed, &len) != ESP_OK || len != sizeof(s_allowed)) {
        memset(&s_allowed, 0, sizeof(s_allowed));
    }

    len = sizeof(s_history);
    if (storage_get_blob(NVS_KEY_DEVICE_HISTORY, &s_history, &len) != ESP_OK || len != sizeof(s_history)) {
        memset(&s_history, 0, sizeof(s_history));
    }

    logger_log(ESP_LOG_INFO, TAG, "Pareamento: %d dispositivo(s) autorizado(s), %d no historico",
               s_allowed.count, s_history.count);
}

bool pairing_is_allowed(const uint8_t mac[6])
{
    bool result = true;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_allowed.count > 0) {
        result = false;
        for (int i = 0; i < s_allowed.count; i++) {
            if (mac_eq(s_allowed.macs[i], mac)) {
                result = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_mutex);
    return result;
}

void pairing_record_device(const uint8_t mac[6], const char *name)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int existing = -1;
    for (int i = 0; i < s_history.count; i++) {
        if (mac_eq(s_history.entries[i].mac, mac)) {
            existing = i;
            break;
        }
    }

    pairing_device_t entry;
    memcpy(entry.mac, mac, 6);
    strlcpy(entry.name, name ? name : "", sizeof(entry.name));
    entry.last_seen_ms = esp_timer_get_time() / 1000;

    if (existing >= 0) {
        /* remove da posição atual para reinserir no topo (mais recente primeiro) */
        for (int i = existing; i < s_history.count - 1; i++) {
            s_history.entries[i] = s_history.entries[i + 1];
        }
        s_history.count--;
    }

    /* insere no topo, empurrando os demais; descarta o mais antigo se cheio */
    int last = (s_history.count < PAIRING_HISTORY_MAX) ? s_history.count : PAIRING_HISTORY_MAX - 1;
    for (int i = last; i > 0; i--) {
        s_history.entries[i] = s_history.entries[i - 1];
    }
    s_history.entries[0] = entry;
    if (s_history.count < PAIRING_HISTORY_MAX) {
        s_history.count++;
    }

    save_history();
    xSemaphoreGive(s_mutex);

    logger_log(ESP_LOG_INFO, TAG, "Dispositivo registrado no historico: %s", name ? name : "?");
}

size_t pairing_get_history(pairing_device_t *out, size_t max_entries)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = s_history.count < max_entries ? s_history.count : max_entries;
    memcpy(out, s_history.entries, n * sizeof(pairing_device_t));
    xSemaphoreGive(s_mutex);
    return n;
}

size_t pairing_get_allowed_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = (size_t)s_allowed.count;
    xSemaphoreGive(s_mutex);
    return n;
}

void pairing_set_allowed(const uint8_t mac[6], bool allowed)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int existing = -1;
    for (int i = 0; i < s_allowed.count; i++) {
        if (mac_eq(s_allowed.macs[i], mac)) {
            existing = i;
            break;
        }
    }

    if (allowed) {
        if (existing < 0 && s_allowed.count < PAIRING_MAX_ALLOWED) {
            memcpy(s_allowed.macs[s_allowed.count], mac, 6);
            s_allowed.count++;
        }
    } else if (existing >= 0) {
        for (int i = existing; i < s_allowed.count - 1; i++) {
            memcpy(s_allowed.macs[i], s_allowed.macs[i + 1], 6);
        }
        s_allowed.count--;
    }

    save_allowed();
    xSemaphoreGive(s_mutex);
}

void pairing_remove_from_history(const uint8_t mac[6])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int existing = -1;
    for (int i = 0; i < s_history.count; i++) {
        if (mac_eq(s_history.entries[i].mac, mac)) {
            existing = i;
            break;
        }
    }

    if (existing >= 0) {
        for (int i = existing; i < s_history.count - 1; i++) {
            s_history.entries[i] = s_history.entries[i + 1];
        }
        s_history.count--;
        save_history();
    }

    xSemaphoreGive(s_mutex);
}

bool pairing_parse_mac(const char *str, uint8_t mac[6])
{
    if (str == NULL) {
        return false;
    }
    unsigned int b[6];
    int n = sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (n != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

void pairing_format_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
