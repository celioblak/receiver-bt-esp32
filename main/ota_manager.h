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

/* Handler HTTP POST /ota/spiffs — recebe a imagem do SPIFFS (a interface web)
 * como corpo bruto e grava direto na partição, reiniciando em seguida. Existe
 * porque antes a interface só podia ser atualizada por cabo USB, enquanto o
 * firmware já subia pela rede.
 *
 * Exemplo:
 *   pio run --target buildfs
 *   curl --data-binary @.pio/build/esp32-a1s/spiffs.bin http://<ip>/ota/spiffs
 */
esp_err_t ota_manager_spiffs_upload_handler(httpd_req_t *req);
