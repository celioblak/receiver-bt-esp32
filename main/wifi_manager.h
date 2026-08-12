#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Wi-Fi STA (credenciais em NVS) com fallback para AP de configuração
 * ("ReceiverBT-Config", sem senha) só quando não há credenciais salvas.
 * Se houver credenciais mas a conexão falhar em WIFI_STA_CONNECT_TIMEOUT_MS,
 * o firmware segue offline — Bluetooth, relé e volume não dependem disso.
 * Quando conectado, inicia mDNS (receiver-bt.local). */

void wifi_manager_init(void);

bool wifi_manager_is_connected(void);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_manager_scan_result_t;

#define WIFI_MANAGER_SCAN_MAX 20

/* Busca (bloqueante, ~1-3s) as redes Wi-Fi visíveis, mais fortes primeiro,
 * sem duplicar SSID repetido em vários canais/APs. Funciona tanto em modo
 * STA (já conectado) quanto no AP de configuração (ver start_ap() —
 * roda em APSTA justamente para permitir isso). Retorna a quantidade
 * encontrada (até max_results). */
size_t wifi_manager_scan(wifi_manager_scan_result_t *out, size_t max_results);

/* true se STA conectado OU AP de configuração ativo — indica que dá pra
 * acessar a interface web. false só no caso "offline de verdade" (STA
 * configurado mas fora do ar). */
bool wifi_manager_network_available(void);

/* Formato "xxx.xxx.xxx.xxx", ou string vazia se não conectado. */
void wifi_manager_get_ip_str(char *out, size_t max_len);

/* IP do gateway (roteador), em network byte order (pronto pra usar em
 * struct sockaddr_in.sin_addr.s_addr) -- usado pelo watchdog de rede (ver
 * main.c) pra checar conectividade de verdade, não só o último evento de
 * conexão/desconexão do driver. false se não conectado. */
bool wifi_manager_get_gateway_ip(uint32_t *out_addr);

/* Atualiza o hostname mDNS (<sanitizado>.local) a partir do nome do
 * dispositivo -- chamar sempre que device_name mudar via /api/config, sem
 * isso o hostname mDNS fica preso no valor de quando o Wi-Fi conectou.
 * Sem efeito se o mDNS ainda não tiver iniciado (aplica no próximo boot). */
void wifi_manager_set_mdns_hostname(const char *device_name);
