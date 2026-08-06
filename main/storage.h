#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Wrapper centralizado sobre NVS (Non-Volatile Storage). Todas as
 * configurações do usuário devem passar por aqui — nunca hardcoded. */

esp_err_t storage_init(void);

esp_err_t storage_set_str(const char *key, const char *value);
esp_err_t storage_get_str(const char *key, char *out, size_t max_len);

esp_err_t storage_set_i32(const char *key, int32_t value);
esp_err_t storage_get_i32(const char *key, int32_t *out, int32_t default_value);

esp_err_t storage_set_blob(const char *key, const void *data, size_t len);
/* Ao chamar, *len deve conter o tamanho do buffer out; ao retornar,
 * contém o tamanho real lido. */
esp_err_t storage_get_blob(const char *key, void *out, size_t *len);

esp_err_t storage_erase_key(const char *key);
