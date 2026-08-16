#include "audio_agc.h"
#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "dlna_renderer.h"
#include "lms_metadata.h"
#include "logger.h"
#include "mqtt_ha.h"
#include "pairing.h"
#include "relay_control.h"
#include "slimproto.h"
#include "storage.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "cJSON.h"
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

/* Nome legivel do motivo do ultimo reset -- esp_reset_reason() le de um
 * registrador que sobrevive ao proprio reset (RTC), entao mesmo um
 * crash/watchdog sem chance de logar nada antes de reiniciar ainda aparece
 * aqui, no BOOT SEGUINTE. Unico jeito de saber a causa real de reinicios
 * inesperados sem acesso a serial (que so mostra o "rst:0x.." bem no
 * inicio, quase sempre perdido antes de alguem conseguir conectar). */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON (energia ligada)";
        case ESP_RST_EXT: return "EXT (pino de reset externo)";
        case ESP_RST_SW: return "SW (esp_restart() chamado pelo proprio firmware)";
        case ESP_RST_PANIC: return "PANIC (crash -- exception/assert)";
        case ESP_RST_INT_WDT: return "INT_WDT (watchdog de interrupcao)";
        case ESP_RST_TASK_WDT: return "TASK_WDT (task travada sem dar yield)";
        case ESP_RST_WDT: return "WDT (outro watchdog)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT (queda de tensao)";
        case ESP_RST_SDIO: return "SDIO";
        default: return "desconhecido";
    }
}

/* cJSON usa malloc()/free() puros por padrao (sem hooks configurados,
 * confirmado: nenhum cJSON_InitHooks() existia em lugar nenhum do
 * firmware) -- e malloc() puro, nesta configuracao (SPIRAM ligado mas sem
 * CONFIG_SPIRAM_USE_MALLOC), fica restrito a RAM interna, igual
 * xTaskCreate()/xRingbufferCreate() sem capabilities explicitas. Toda
 * resposta JSON da API (web_server.c: /api/status, /api/logs, /api/config,
 * /api/devices...) passa por cJSON -- /api/logs em particular monta ate
 * ~100 entradas de uma vez, uma unica alocacao grande (~15-20KB) de RAM
 * interna a cada chamada. Confirmado como suspeito real (2026-08-14): as
 * checagens repetidas de /api/logs durante o dia inteiro de depuracao
 * provavelmente contribuiram pros travamentos do servidor web observados.
 * Redireciona cJSON pra PSRAM, mesmo padrao ja usado pro buffer de log e
 * pelos ring buffers de audio. */
static void *cjson_psram_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void cjson_psram_free(void *ptr)
{
    heap_caps_free(ptr);
}

void app_main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = cjson_psram_malloc, .free_fn = cjson_psram_free};
    cJSON_InitHooks(&hooks);

    logger_init();
    ESP_ERROR_CHECK(storage_init());

    logger_log(ESP_LOG_INFO, TAG, "Receiver Bluetooth DIY - firmware %s", FW_VERSION);
    logger_log(ESP_LOG_WARN, TAG, "Motivo do reset anterior: %s", reset_reason_name(esp_reset_reason()));

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
        lms_metadata_init();
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
        /* "maior bloco": diferente do total livre -- e o que realmente
         * decide se uma alocacao grande e CONTIGUA (ex.: stack de uma task
         * nova) vai conseguir ou nao. Confirmado ao vivo: total livre de
         * ~18KB (RAM interna) ainda assim falhou ao pedir 10KB pra stack da
         * task do decoder FLAC -- ou seja, o heap estava mais fragmentado
         * do que o total livre sozinho deixava parecer. */
        logger_log(ESP_LOG_INFO, TAG,
                   "heap livre: %u bytes (interna: %u, minima ja vista: %u, maior bloco: %u)",
                   (unsigned)esp_get_free_heap_size(),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        mqtt_ha_publish_state();
        network_watchdog_check();
        bt_audio_check_discoverable_timeout();
        /* 30s (nao 10s): cada publicacao MQTT e uma transmissao WiFi, e
         * atividade de radio (WiFi ou BT) acopla ruido audivel no estagio
         * analogico do ES8388 nesta placa (ver README). Trade-off: esta e a
         * unica fonte de atualizacao periodica do estado no Home Assistant
         * (nao ha publish orientado a evento em volume/playing ainda), entao
         * o estado la fica ate 30s desatualizado em vez de ate 10s. */
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
