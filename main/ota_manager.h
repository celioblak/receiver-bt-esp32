#pragma once

#include "esp_http_server.h"

/* Handler HTTP POST /ota — recebe o novo firmware (.bin) como corpo bruto
 * da requisição (application/octet-stream, não multipart — mais simples
 * de implementar e de usar via curl --data-binary), grava na partição OTA
 * inativa via esp_ota_ops e reinicia automaticamente se der certo.
 *
 * Exemplo de uso: curl --data-binary @firmware.bin http://receiver-bt.local/ota
 */
esp_err_t ota_manager_upload_handler(httpd_req_t *req);
