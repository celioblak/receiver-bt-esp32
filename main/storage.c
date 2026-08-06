#include "storage.h"

#include <string.h>
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS sem espaço/versão antiga, apagando e reinicializando");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }

    return err;
}

esp_err_t storage_set_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t storage_get_str(const char *key, char *out, size_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, key, out, &max_len);
        nvs_close(handle);
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t storage_set_i32(const char *key, int32_t value)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t storage_get_i32(const char *key, int32_t *out, int32_t default_value)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_i32(handle, key, out);
        nvs_close(handle);
    }

    xSemaphoreGive(s_mutex);

    if (err != ESP_OK) {
        *out = default_value;
    }

    return err;
}

esp_err_t storage_set_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, key, data, len);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t storage_get_blob(const char *key, void *out, size_t *len)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, key, out, len);
        nvs_close(handle);
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t storage_erase_key(const char *key)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, key);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    xSemaphoreGive(s_mutex);
    return err;
}
