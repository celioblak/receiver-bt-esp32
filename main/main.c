#include "audio_agc.h"
#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "dlna_renderer.h"
#include "logger.h"
#include "mqtt_ha.h"
#include "pairing.h"
#include "relay_control.h"
#include "slimproto.h"
#include "storage.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include "lwip/sockets.h"

static const char *TAG = "main";

/* Watchdog de rede: o WiFi as vezes fica "travado" sem gerar nenhum evento
 * de desconexao (relatado ja antes deste firmware existir) -- o main loop
 * continua rodando normalmente (heap sendo logado certinho), mas ping/HTTP
 * param de responder e so um reset fisico resolve. wifi_manager_is_
 * connected() so reflete o ultimo evento do driver, entao nao detecta esse
 * tipo de trava silenciosa. Este watchdog checa conectividade de verdade
 * (tenta abrir uma conexao TCP com o roteador) e reinicia sozinho se
 * confirmar que esta morto ha varios ciclos seguidos, em vez de depender de
 * alguem notar e resetar na mao. */
#define NETWORK_WATCHDOG_MAX_FAILURES 3

static bool network_watchdog_probe_gateway(void)
{
    uint32_t gw_addr;
    if (!wifi_manager_get_gateway_ip(&gw_addr)) {
        return true; /* sem IP -- nao e o cenario que este watchdog cobre */
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = gw_addr;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return true; /* falha ao criar socket nao prova rede morta */
    }

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Sucesso OU recusa de conexao (ECONNREFUSED, resposta rapida) provam
     * que a rede esta viva -- so timeout total (sem resposta nenhuma)
     * indica rede realmente morta. */
    int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    bool alive = (ret == 0) || (errno == ECONNREFUSED);
    close(sock);
    return alive;
}

static void network_watchdog_check(void)
{
    if (!wifi_manager_is_connected()) {
        return; /* desconexao ja detectada pelo evento do driver, que reconecta sozinho */
    }

    static int s_consecutive_failures = 0;

    if (network_watchdog_probe_gateway()) {
        s_consecutive_failures = 0;
        return;
    }

    s_consecutive_failures++;
    logger_log(ESP_LOG_WARN, TAG, "Watchdog de rede: roteador nao responde (%d/%d)",
               s_consecutive_failures, NETWORK_WATCHDOG_MAX_FAILURES);

    if (s_consecutive_failures >= NETWORK_WATCHDOG_MAX_FAILURES) {
        logger_log(ESP_LOG_ERROR, TAG,
                   "Watchdog de rede: sem resposta ha %d ciclos -- reiniciando",
                   s_consecutive_failures);
        vTaskDelay(pdMS_TO_TICKS(200)); /* da tempo do log sair antes do reset */
        esp_restart();
    }
}

void app_main(void)
{
    logger_init();
    ESP_ERROR_CHECK(storage_init());

    logger_log(ESP_LOG_INFO, TAG, "Receiver Bluetooth DIY - firmware %s", FW_VERSION);

    ESP_ERROR_CHECK(audio_codec_init());

    /* Muda o DAC já na inicialização: sem áudio real chegando ainda, ruído
     * digital/RF (WiFi, negociação Bluetooth) vazaria pelo fone o tempo
     * todo. Só desmuda quando o A2DP realmente começa a tocar (bt_audio.c). */
    audio_codec_set_mute(true);

    pairing_init();
    relay_control_init();
    /* NAO chamar audio_codec_play_test_tone() aqui -- ligar rele + I2S tao
     * cedo no boot (antes do WiFi/BT estabilizarem) e suspeito de causar o
     * "nao liga sozinho na tomada, so com serial conectado" (a serial pode
     * estar mascarando um pico de corrente/queda de tensao que a energia
     * USB por si so nao segura). O bipe continua disponivel sob demanda via
     * /api/system/beep, sem esse risco no caminho critico do boot. */
    bt_audio_init();
    audio_agc_init();
    wifi_manager_init();

    if (wifi_manager_network_available()) {
        web_server_start();
        dlna_renderer_init();
        slimproto_init();
    }
    mqtt_ha_init();

    while (1) {
        /* esp_get_free_heap_size() inclui os 4MB de PSRAM e sempre parece
         * saudavel -- mas WiFi/BT dependem especificamente de um pool bem
         * menor de RAM INTERNA (~178KB, ver heap_init no boot), que a PSRAM
         * nao pode suprir. Investigando queda espontanea de WiFi (sem
         * crash, sem brownout no log) que ja era relatada antes deste
         * firmware existir -- suspeita de vazamento/fragmentacao especifico
         * da RAM interna que o heap total nao revela. O "minimo historico"
         * pega quedas breves que o valor atual, medido só a cada 30s, pode
         * nao capturar. */
        logger_log(ESP_LOG_INFO, TAG,
                   "heap livre: %u bytes (interna: %u, minima ja vista: %u)",
                   (unsigned)esp_get_free_heap_size(),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        mqtt_ha_publish_state();
        network_watchdog_check();
        /* 30s (nao 10s): cada publicacao MQTT e uma transmissao WiFi, e
         * atividade de radio (WiFi ou BT) acopla ruido audivel no estagio
         * analogico do ES8388 nesta placa (ver README). Trade-off: esta e a
         * unica fonte de atualizacao periodica do estado no Home Assistant
         * (nao ha publish orientado a evento em volume/playing ainda), entao
         * o estado la fica ate 30s desatualizado em vez de ate 10s. */
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
