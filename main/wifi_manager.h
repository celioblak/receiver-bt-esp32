#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Wi-Fi STA (credenciais em NVS) com fallback para AP de configuração
 * ("ReceiverBT-Config", sem senha) só quando não há credenciais salvas.
 * Se houver credenciais mas a conexão falhar em WIFI_STA_CONNECT_TIMEOUT_MS,
 * o firmware segue offline — Bluetooth, relé e volume não dependem disso.
 * Quando conectado, inicia mDNS (receiver-bt.local). */

void wifi_manager_init(void);

bool wifi_manager_is_connected(void);

/* Formato "xxx.xxx.xxx.xxx", ou string vazia se não conectado. */
void wifi_manager_get_ip_str(char *out, size_t max_len);
