#pragma once

/* Monta o SPIFFS (interface web em spiffs_image/) e sobe o esp_http_server
 * com a API REST (/api/status, /api/config, /api/volume, /api/logs).
 * Chamar só quando há rede disponível (STA conectado ou AP de config
 * ativo) — ver wifi_manager_network_available(). */

void web_server_start(void);
