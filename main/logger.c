#include "logger.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* LOGGER_MAX_ENTRIES(100) * sizeof(log_entry_t)(~144 bytes) = ~14KB -- so
 * dados (nunca stack de execucao), lidos/escritos so via logger_log()/
 * logger_get_entries() com mutex, entao seguro em PSRAM. Antes era um
 * array static (RAM interna, direto no .bss) -- de longe o maior
 * consumidor unico de RAM interna permanente que achamos ao vasculhar o
 * firmware atras de folga (ver memoria project_wifi_instability_
 * unresolved: RAM interna cronica em ~9-16KB depois do stack estatico do
 * decoder FLAC). Alocado uma vez em logger_init(), que roda bem no inicio
 * do app_main() -- PSRAM ja esta no pool do heap allocator nesse ponto
 * (mapeada durante cpu_start, antes de app_main ser chamado). */
static log_entry_t *s_entries = NULL;
static size_t s_head = 0;   /* índice da próxima escrita */
static size_t s_count = 0;  /* quantidade de entradas válidas */
static SemaphoreHandle_t s_mutex = NULL;

void logger_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_entries == NULL) {
        s_entries = heap_caps_malloc(sizeof(log_entry_t) * LOGGER_MAX_ENTRIES, MALLOC_CAP_SPIRAM);
    }
    s_head = 0;
    s_count = 0;
}

void logger_log(esp_log_level_t level, const char *tag, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    char formatted[LOGGER_MSG_MAX_LEN];
    vsnprintf(formatted, sizeof(formatted), fmt, args);
    va_end(args);

    switch (level) {
        case ESP_LOG_ERROR:   ESP_LOGE(tag, "%s", formatted); break;
        case ESP_LOG_WARN:    ESP_LOGW(tag, "%s", formatted); break;
        case ESP_LOG_INFO:    ESP_LOGI(tag, "%s", formatted); break;
        case ESP_LOG_DEBUG:   ESP_LOGD(tag, "%s", formatted); break;
        case ESP_LOG_VERBOSE: ESP_LOGV(tag, "%s", formatted); break;
        default: break;
    }

    if (s_mutex == NULL || s_entries == NULL) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        log_entry_t *entry = &s_entries[s_head];
        entry->timestamp_ms = esp_timer_get_time() / 1000;
        entry->level = level;
        strlcpy(entry->msg, formatted, sizeof(entry->msg));

        s_head = (s_head + 1) % LOGGER_MAX_ENTRIES;
        if (s_count < LOGGER_MAX_ENTRIES) {
            s_count++;
        }

        xSemaphoreGive(s_mutex);
    }
}

size_t logger_get_entries(log_entry_t *out_buf, size_t max_entries)
{
    if (s_mutex == NULL || s_entries == NULL) {
        return 0;
    }

    size_t copied = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t n = s_count < max_entries ? s_count : max_entries;
        /* entrada mais antiga está em (s_head - s_count + LOGGER_MAX_ENTRIES) % LOGGER_MAX_ENTRIES */
        size_t start = (s_head + LOGGER_MAX_ENTRIES - s_count) % LOGGER_MAX_ENTRIES;

        for (size_t i = 0; i < n; i++) {
            size_t idx = (start + i) % LOGGER_MAX_ENTRIES;
            out_buf[i] = s_entries[idx];
        }
        copied = n;

        xSemaphoreGive(s_mutex);
    }

    return copied;
}
