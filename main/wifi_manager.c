#include "wifi_manager.h"

#include <string.h>

#include "config.h"
#include "logger.h"
#include "storage.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static bool s_connected = false;
static bool s_ap_mode = false;
static esp_netif_ip_info_t s_ip_info;

/* Hostname mDNS precisa ser um nome DNS valido (so letras/digitos/hifen) --
 * o nome do dispositivo (Bluetooth) aceita qualquer coisa (espacos, acentos,
 * emoji), entao sanitiza em vez de usar direto. */
static void sanitize_hostname(const char *device_name, char *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; device_name[i] != '\0' && j < out_size - 1; i++) {
        char c = device_name[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (c >= 'A' && c <= 'Z') {
            out[j++] = (char)(c - 'A' + 'a');
        } else if (j > 0 && out[j - 1] != '-') {
            out[j++] = '-';
        }
    }
    while (j > 0 && out[j - 1] == '-') {
        j--; /* sem hifen sobrando no final (ex.: nome so com espacos/acentos) */
    }
    out[j] = '\0';
    if (j == 0) {
        strlcpy(out, MDNS_HOSTNAME, out_size);
    }
}

static bool s_mdns_started = false;

void wifi_manager_set_mdns_hostname(const char *device_name)
{
    if (!s_mdns_started) {
        return; /* aplicado quando o mDNS realmente iniciar (ver start_mdns) */
    }
    char hostname[40];
    sanitize_hostname(device_name, hostname, sizeof(hostname));
    mdns_hostname_set(hostname);
    logger_log(ESP_LOG_INFO, TAG, "mDNS atualizado: %s.local", hostname);
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "falha ao iniciar mDNS");
        return;
    }
    s_mdns_started = true;

    char device_name[32];
    if (storage_get_str(NVS_KEY_DEVICE_NAME, device_name, sizeof(device_name)) != ESP_OK) {
        strlcpy(device_name, FW_DEVICE_NAME_DEFAULT, sizeof(device_name));
    }
    char hostname[40];
    sanitize_hostname(device_name, hostname, sizeof(hostname));

    mdns_hostname_set(hostname);
    mdns_instance_name_set("Receiver Bluetooth DIY");

    /* Ate aqui so resolvia o hostname (<nome>.local) -- nunca anunciava um
     * SERVICO mDNS de verdade, entao nenhum control point/integracao que
     * dependa de descoberta automatica via zeroconf (ex.: config_flow do
     * componente customizado da Home Assistant, ver
     * docs/home_assistant_custom_component_prompt.md) conseguia achar o
     * dispositivo sozinho -- so digitando o IP/host na mao. "_receiverbt"
     * e o nome de servico que esse componente espera encontrar; "_http"
     * tambem e anunciado por ser generico/util (qualquer navegador/
     * ferramenta de descoberta mDNS padrao ja reconhece). */
    mdns_txt_item_t txt[] = {
        {"fw_version", FW_VERSION},
    };
    mdns_service_add(NULL, "_receiverbt", "_tcp", 80, txt, 1);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    logger_log(ESP_LOG_INFO, TAG, "mDNS ativo: %s.local (servico _receiverbt._tcp anunciado)", hostname);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        logger_log(ESP_LOG_WARN, TAG, "Wi-Fi desconectado, tentando reconectar...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_ip_info = event->ip_info;
        s_connected = true;
        logger_log(ESP_LOG_INFO, TAG, "Wi-Fi conectado, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        start_mdns();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_ap(void)
{
    esp_netif_create_default_wifi_ap();
    /* STA "sombra" sem se conectar a nada — só para o rádio ter uma
     * interface capaz de escanear (wifi_manager_scan) enquanto o AP de
     * configuração está no ar, sem precisar sair dele. */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, WIFI_AP_SSID_DEFAULT, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(WIFI_AP_SSID_DEFAULT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_mode = true;
    logger_log(ESP_LOG_INFO, TAG, "Sem credenciais salvas — AP de configuracao ativo: %s", WIFI_AP_SSID_DEFAULT);
}

static void start_sta_and_wait(const char *ssid, const char *pass)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Sem power-save: o radio WiFi acorda periodicamente (~beacon interval,
     * ver "wifi:pm start" no log) pra checar por dados quando em modem
     * sleep, e cada ciclo liga/desliga o radio -- candidato a fonte do
     * ruido periodico ("toto toto") no ES8388, independente do Bluetooth.
     * Ja foi tentado antes e revertido por piorar disponibilidade de heap,
     * mas isso era pre-PSRAM (heap era ~11-26KB); agora sobra ~4MB, entao
     * o custo do heap nao se aplica mais. Custo real: mais consumo de
     * energia (irrelevante, fonte externa). */
    esp_wifi_set_ps(WIFI_PS_NONE);

    logger_log(ESP_LOG_INFO, TAG, "Conectando ao Wi-Fi \"%s\"...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(WIFI_STA_CONNECT_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        logger_log(ESP_LOG_WARN, TAG,
                   "Nao conectou em %d ms, seguindo offline (Bluetooth e rele funcionam normalmente)",
                   WIFI_STA_CONNECT_TIMEOUT_MS);
    }
}

void wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = storage_get_str(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid));

    if (err != ESP_OK || strlen(ssid) == 0) {
        start_ap();
        return;
    }

    storage_get_str(NVS_KEY_WIFI_PASS, pass, sizeof(pass));
    start_sta_and_wait(ssid, pass);
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

bool wifi_manager_network_available(void)
{
    return s_connected || s_ap_mode;
}

size_t wifi_manager_scan(wifi_manager_scan_result_t *out, size_t max_results)
{
    if (out == NULL || max_results == 0) {
        return 0;
    }

    wifi_scan_config_t scan_cfg = {0};
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return 0;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return 0;
    }

    wifi_ap_record_t *records = calloc(found, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        return 0;
    }
    esp_wifi_scan_get_ap_records(&found, records);

    size_t count = 0;
    for (uint16_t i = 0; i < found && count < max_results; i++) {
        if (records[i].ssid[0] == '\0') {
            continue; /* rede oculta, sem SSID pra mostrar/preencher */
        }
        bool dup = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, (const char *)records[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue; /* mesmo SSID visto em outro canal/AP — a lista ja vem ordenada por sinal */
        }
        strlcpy(out[count].ssid, (const char *)records[i].ssid, sizeof(out[count].ssid));
        out[count].rssi = records[i].rssi;
        out[count].secure = (records[i].authmode != WIFI_AUTH_OPEN);
        count++;
    }

    free(records);
    return count;
}

void wifi_manager_get_ip_str(char *out, size_t max_len)
{
    if (out == NULL || max_len == 0) {
        return;
    }
    if (!s_connected) {
        out[0] = '\0';
        return;
    }
    snprintf(out, max_len, IPSTR, IP2STR(&s_ip_info.ip));
}

bool wifi_manager_get_gateway_ip(uint32_t *out_addr)
{
    if (out_addr == NULL || !s_connected) {
        return false;
    }
    *out_addr = s_ip_info.gw.addr;
    return true;
}
