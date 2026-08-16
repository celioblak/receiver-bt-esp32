#include "lms_cli.h"

#include "config.h"
#include "logger.h"
#include "slimproto.h"
#include "storage.h"

#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "lms_cli";

#define LMS_CLI_PORT 9090

/* Player ID no protocolo LMS é o endereço MAC do cliente Slimproto (mesmo
 * MAC WiFi STA usado no HELO, ver slimproto.c) formatado como texto, com os
 * ':' escapados como %3A -- confirmado ao vivo contra o Music Assistant
 * (ver docs/music_assistant_integration.md). */
static void player_id_encoded(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, out_len, "%02x%%3A%02x%%3A%02x%%3A%02x%%3A%02x%%3A%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char *transport_cmd_to_lms(const char *cmd)
{
    if (strcmp(cmd, "play") == 0) {
        return "play";
    }
    if (strcmp(cmd, "pause") == 0) {
        return "pause 1";
    }
    /* "playpause" e tratado a parte em lms_cli_send_transport() -- ver ali
     * (bare "pause" sem argumento nao alterna de verdade nesse servidor,
     * confirmado ao vivo em 2026-08-14: isplaying ficava travado em 0
     * depois de um "pause" sem argumento). */
    if (strcmp(cmd, "stop") == 0) {
        return "stop";
    }
    if (strcmp(cmd, "next") == 0) {
        return "playlist index +1";
    }
    if (strcmp(cmd, "previous") == 0) {
        /* NAO "playlist index -1" -- confirmado lendo o fonte real do
         * aioslimproto (providers/squeezelite/../aioslimproto/cli.py,
         * _handle_playlist()) rodando dentro do container do MA: o handler
         * so trata "playlist index +1" (com um comentario explicito "we
         * only handle playlist index +1 - the rest is forwarded") e da
         * NotImplementedError pra qualquer outro valor de index, incluindo
         * -1. Por isso "previous" nunca teve efeito real mesmo depois da
         * correcao do player_id truncado (2026-08-14) -- nao era falha de
         * conexao/player_id, era comando sem handler nenhum do lado do
         * servidor. O comando que de fato funciona (visto em
         * providers/squeezelite/player.py, _handle_player_cli_event) e um
         * evento de botao: "button rew" (ou "button jump_rew", mesmo
         * efeito) chama mass.player_queues.previous() diretamente. */
        return "button rew";
    }
    return NULL;
}

/* Uma tentativa completa: abre conexao nova, manda o comando, le o eco,
 * fecha. Mesmo padrao ja validado (11h+ de uptime estavel) -- SEM estado
 * persistente entre chamadas (isso foi tentado e revertido no mesmo dia
 * depois de causar um PANIC + travamento, ver main/lms_cli.h). */
static bool lms_cli_attempt(const char *host, const char *player_id, const char *suffix, const char *cmd)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", LMS_CLI_PORT);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        logger_log(ESP_LOG_WARN, TAG, "lms_cli: falha ao resolver %s", host);
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        freeaddrinfo(res);
        return false;
    }

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool ok = false;
    if (connect(sock, res->ai_addr, res->ai_addrlen) == 0) {
        /* 24 (antigo) era pequeno demais: 6 bytes de MAC "XX" (2) + "%3A"
         * (3) entre eles (5 separadores) = 6*2+5*3 = 27 caracteres + '\0'
         * = 28 bytes precisos. Com 24, o snprintf em player_id_encoded()
         * TRUNCAVA silenciosamente os ultimos 4 caracteres ("3Aa8" do
         * ultimo byte do MAC) -- o player_id enviado nunca batia com
         * nenhum player de verdade no MA, entao TODO comando de midia
         * era aceito pela conexao TCP mas silenciosamente ignorado pelo
         * servidor. Confirmado ao vivo em 2026-08-14 logando a linha exata
         * enviada -- essa e a causa raiz real de toda a investigacao de
         * controles de midia daquele dia. */
        char line[96];
        int line_len = snprintf(line, sizeof(line), "%s %s\n", player_id, suffix);
        bool sent = (line_len > 0) && (send(sock, line, (size_t)line_len, 0) == line_len);
        /* CRITICO: sem isso, fechavamos o socket imediatamente apos o send()
         * ter sucesso -- mas send() so garante que o dado foi entregue pro
         * stack TCP local, nao que o SERVIDOR ja leu/processou. Confirmado
         * ao vivo em 2026-08-14: o aioslimproto parece descartar o comando
         * se a conexao fecha rapido demais, antes do proprio loop de
         * eventos ler e processar a linha. Le a linha de eco que o LMS CLI
         * sempre manda de volta antes de fechar, dando tempo do servidor
         * processar de verdade -- e (2026-08-16) trata a AUSENCIA do eco
         * como falha real, nao so como "sem confirmacao": log ao vivo
         * mostrou comandos "enviados com sucesso" sem efeito nenhum no MA,
         * consistente com o rev roundtrip TCP tendo funcionado mas o eco
         * nunca tendo chegado a tempo (disputa de radio WiFi/BT com o
         * audio, ver memoria do projeto sobre engasgo) -- so contar como
         * sucesso real quando o eco de fato voltou. */
        char echo[96] = {0};
        if (sent) {
            int n = recv(sock, echo, sizeof(echo) - 1, 0);
            if (n > 0) {
                echo[n] = '\0';
                ok = true;
            }
        }
        logger_log(ESP_LOG_INFO, TAG, "lms_cli: comando '%s' -> %s | linha='%s' eco='%s'",
                   cmd, ok ? "enviado" : "falhou", line, echo);
    } else {
        logger_log(ESP_LOG_WARN, TAG, "lms_cli: falha ao conectar em %s:%d", host, LMS_CLI_PORT);
    }

    close(sock);
    freeaddrinfo(res);
    return ok;
}

bool lms_cli_send_transport(const char *cmd)
{
    /* "playpause" precisa de estado explicito (ver comentario em
     * transport_cmd_to_lms) -- usa o que o nosso proprio STAT/slim_playing
     * ja sabe (vem de faixa/STMs/STMd reais, nao de suposicao) pra decidir
     * "pause 1" ou "pause 0" em vez de confiar num toggle que nao funciona
     * nesse servidor. */
    char suffix_buf[16];
    const char *suffix;
    if (strcmp(cmd, "playpause") == 0) {
        slimproto_status_t slim;
        slimproto_get_status(&slim);
        strlcpy(suffix_buf, slim.playing ? "pause 1" : "pause 0", sizeof(suffix_buf));
        suffix = suffix_buf;
    } else {
        suffix = transport_cmd_to_lms(cmd);
    }
    if (suffix == NULL) {
        return false;
    }

    char host[64];
    if (storage_get_str(NVS_KEY_SLIM_HOST, host, sizeof(host)) != ESP_OK || host[0] == '\0') {
        logger_log(ESP_LOG_WARN, TAG, "lms_cli: host do Music Assistant nao configurado");
        return false;
    }

    char player_id[32];
    player_id_encoded(player_id, sizeof(player_id));

    /* Folga antes de abrir a conexao, se uma transicao de sessao (troca de
     * faixa) acabou de acontecer -- achado ao vivo com captura serial real
     * em 2026-08-16: tres crashes distintos e confirmados (reuso de
     * stack de task, corrida de escrita na NVS, e um use-after-free real
     * dentro do proprio lwIP -- "assert failed: xQueueGenericSend ...
     * (pxQueue)", tcpip_thread no meio de lwip_netconn_do_write) sempre
     * aconteceram durante ou logo apos a rajada de strm 'f'->'q'->'s' que
     * um "next"/"previous" dispara. Fator comum: nossa conexao nova (porta
     * 9090) e a reconstrucao da conexao de dados do Slimproto disputando as
     * mesmas estruturas internas do lwIP ao mesmo tempo. Isso nao elimina a
     * causa raiz exata (dentro do lwIP, fora do nosso controle direto), mas
     * reduz bastante a chance de coincidir -- 150ms e uma folga pequena o
     * bastante pra nao atrasar o comando de forma perceptivel. */
    int64_t elapsed_us = esp_timer_get_time() - slimproto_get_last_transition_time_us();
    const int64_t min_gap_us = 150 * 1000;
    if (elapsed_us >= 0 && elapsed_us < min_gap_us) {
        vTaskDelay(pdMS_TO_TICKS((min_gap_us - elapsed_us) / 1000));
    }

    return lms_cli_attempt(host, player_id, suffix, cmd);
}
