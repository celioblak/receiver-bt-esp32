#include "lms_cli.h"

#include "config.h"
#include "logger.h"
#include "storage.h"

#include "esp_wifi.h"
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
    if (strcmp(cmd, "playpause") == 0) {
        /* "pause" sem argumento alterna sozinho no LMS -- não precisamos
         * rastrear o último estado como bt_audio_media_control() faz pro
         * AVRCP (que não tem toggle nativo). */
        return "pause";
    }
    if (strcmp(cmd, "stop") == 0) {
        return "stop";
    }
    if (strcmp(cmd, "next") == 0) {
        return "playlist index +1";
    }
    if (strcmp(cmd, "previous") == 0) {
        return "playlist index -1";
    }
    return NULL;
}

bool lms_cli_send_transport(const char *cmd)
{
    const char *suffix = transport_cmd_to_lms(cmd);
    if (suffix == NULL) {
        return false;
    }

    char host[64];
    if (storage_get_str(NVS_KEY_SLIM_HOST, host, sizeof(host)) != ESP_OK || host[0] == '\0') {
        logger_log(ESP_LOG_WARN, TAG, "lms_cli: host do Music Assistant nao configurado");
        return false;
    }

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
        char player_id[24];
        player_id_encoded(player_id, sizeof(player_id));
        char line[96];
        int line_len = snprintf(line, sizeof(line), "%s %s\n", player_id, suffix);
        ok = (line_len > 0) && (send(sock, line, (size_t)line_len, 0) == line_len);
        logger_log(ESP_LOG_INFO, TAG, "lms_cli: comando '%s' -> %s", cmd, ok ? "enviado" : "falhou ao enviar");
    } else {
        logger_log(ESP_LOG_WARN, TAG, "lms_cli: falha ao conectar em %s:%d", host, LMS_CLI_PORT);
    }

    close(sock);
    freeaddrinfo(res);
    return ok;
}
