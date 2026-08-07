#include "ota_manager.h"

#include "logger.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_manager";

esp_err_t ota_manager_upload_handler(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo vazio");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem particao OTA disponivel");
        return ESP_FAIL;
    }

    logger_log(ESP_LOG_INFO, TAG, "OTA: recebendo %d bytes para a particao \"%s\"...",
               req->content_len, update_partition->label);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin falhou: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin falhou");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    int total_written = 0;

    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int recv_len = httpd_req_recv(req, buf, to_read);

        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue; /* tenta de novo */
        }
        if (recv_len <= 0) {
            esp_ota_end(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "erro ao receber dados");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write falhou: %s", esp_err_to_name(err));
            esp_ota_end(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write falhou");
            return ESP_FAIL;
        }

        remaining -= recv_len;
        total_written += recv_len;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        const char *msg = (err == ESP_ERR_OTA_VALIDATE_FAILED) ? "imagem de firmware invalida" : "esp_ota_end falhou";
        ESP_LOGE(TAG, "%s: %s", msg, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition falhou: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_set_boot_partition falhou");
        return ESP_FAIL;
    }

    logger_log(ESP_LOG_INFO, TAG, "OTA concluido (%d bytes), reiniciando em breve...", total_written);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"OTA concluido, reiniciando...\"}");

    /* dá tempo da resposta HTTP sair antes de reiniciar */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK; /* nunca chega aqui */
}
