#include "logger.h"

#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static log_entry_t s_entries[LOGGER_MAX_ENTRIES];
static size_t s_head = 0;   /* índice da próxima escrita */
static size_t s_count = 0;  /* quantidade de entradas válidas */
static SemaphoreHandle_t s_mutex = NULL;

void logger_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
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

    if (s_mutex == NULL) {
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
    if (s_mutex == NULL) {
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
