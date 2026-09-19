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

/* Potência do sinal do Wi-Fi em dBm (negativo; quanto mais perto de zero,
 * melhor). Devolve false se não houver conexão. Referência prática:
 * -50 ótimo, -60 bom, -70 aceitável, -80 no limite, abaixo disso instável. */
bool wifi_manager_get_rssi(int *rssi_dbm);

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

/* Desliga o rádio Wi-Fi por um tempo limitado (diagnóstico: isolar se algum
 * ruído/interferência no áudio vem do Wi-Fi ou do Bluetooth -- com o Wi-Fi
 * fora do ar, só o Bluetooth continua usando o rádio do chip). Depois do
 * prazo, reinicia o dispositivo sozinho pra restaurar tudo (Wi-Fi/HTTP/MQTT)
 * -- não fica preso sem rede esperando um comando que não vai chegar, já que
 * sem Wi-Fi não tem como mandar um comando de volta. Bluetooth/áudio/relé
 * continuam funcionando normalmente durante a janela. */
void wifi_manager_disable_temporarily(uint32_t duration_s);

/* Segundos restantes da janela acima, ou 0 se não estiver ativa. */
uint32_t wifi_manager_disable_remaining_s(void);

/* Liga/desliga o rádio Wi-Fi manualmente (botão físico KEY1, ver
 * button_diag.c) -- ao contrário de wifi_manager_disable_temporarily(),
 * fica desligado até alguém apertar de novo, não só por um tempo fixo.
 * Bluetooth/áudio/relé continuam funcionando normalmente desligado. Tem uma
 * rede de segurança: se ninguém apertar de novo em
 * WIFI_RADIO_OFF_SAFETY_S, reinicia sozinho (evita ficar preso sem Wi-Fi
 * pra sempre por esquecimento, já que sem Wi-Fi não tem outro jeito de
 * religar remotamente). */
void wifi_manager_radio_toggle(void);

/* true se o rádio foi desligado manualmente (wifi_manager_radio_toggle) e
 * ainda não foi religado. */
bool wifi_manager_radio_is_off(void);
