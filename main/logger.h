#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_log.h"
#include "config.h"

typedef struct {
    int64_t timestamp_ms;
    esp_log_level_t level;
    char msg[LOGGER_MSG_MAX_LEN];
} log_entry_t;

/* Ring buffer em memória com as últimas LOGGER_MAX_ENTRIES mensagens,
 * usado pelo endpoint GET /api/logs. Espelha a mensagem no log padrão
 * do ESP-IDF (ESP_LOG*) além de guardar no buffer. */

void logger_init(void);

void logger_log(esp_log_level_t level, const char *tag, const char *fmt, ...);

/* Copia até max_entries do buffer (da mais antiga para a mais recente)
 * para out_buf. Retorna a quantidade efetivamente copiada. */
size_t logger_get_entries(log_entry_t *out_buf, size_t max_entries);
