#include "ota_manager.h"

#include "logger.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
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


/* POST /ota/spiffs -- mesma ideia do /ota, mas para a particao do SPIFFS
 * (a interface web: HTML/CSS/JS de spiffs_image/).
 *
 * Por que existe: ate 2026-08-29 a interface so podia ser atualizada com o
 * aparelho ligado por cabo USB (`pio run --target uploadfs`), enquanto o
 * firmware ja subia pela rede. Isso travou varias vezes uma correcao de
 * pagina que estava pronta e nao podia chegar no aparelho -- o dispositivo
 * mora longe do computador. Pedido do Celio.
 *
 * Recebe a imagem crua gerada pelo PlatformIO em
 * .pio/build/esp32-a1s/spiffs.bin e a grava direto na particao, sem passar
 * pelo esp_ota_ops (que so sabe lidar com particoes de app). O SPIFFS e
 * desmontado antes: escrever debaixo de um sistema de arquivos montado
 * corromperia o cache dele. Reinicia no fim pra remontar limpo.
 *
 * Uso:
 *   pio run --target buildfs
 *   curl --data-binary @.pio/build/esp32-a1s/spiffs.bin  *        http://<ip>/ota/spiffs
 */
esp_err_t ota_manager_spiffs_upload_handler(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo vazio");
        return ESP_FAIL;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (part == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "particao spiffs nao encontrada");
        return ESP_FAIL;
    }
    if (req->content_len > (int)part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "imagem maior que a particao");
        return ESP_FAIL;
    }

    logger_log(ESP_LOG_INFO, TAG,
               "OTA SPIFFS: recebendo %d bytes para \"%s\" (%u bytes)...",
               req->content_len, part->label, (unsigned)part->size);

    /* Desmonta antes de escrever -- ver comentario acima. Ignora o erro: se
     * nao estava montado, seguir em frente e o que queremos. */
    esp_vfs_spiffs_unregister(NULL);

    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase da particao spiffs falhou: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "erase falhou");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    size_t offset = 0;

    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int recv_len = httpd_req_recv(req, buf, to_read);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv_len <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "erro ao receber dados");
            return ESP_FAIL;
        }
        err = esp_partition_write(part, offset, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "escrita na particao spiffs falhou: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "escrita falhou");
            return ESP_FAIL;
        }
        offset += recv_len;
        remaining -= recv_len;
    }

    logger_log(ESP_LOG_INFO, TAG, "OTA SPIFFS: %u bytes gravados, reiniciando...",
               (unsigned)offset);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Interface atualizada, reiniciando...\"}");

    vTaskDelay(pdMS_TO_TICKS(500)); /* deixa a resposta sair antes do reset */
    esp_restart();
    return ESP_OK;
}
