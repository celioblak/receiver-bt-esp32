#include "slimproto.h"

#include <string.h>
#include <stdlib.h>

#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "logger.h"
#include "relay_control.h"
#include "storage.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "slimproto";

#define HEARTBEAT_INTERVAL_US   (5000 * 1000)
#define RECONNECT_DELAY_MS      5000
#define MAX_CTRL_PAYLOAD        768   /* strm (24 + cabecalho HTTP) cabe folgado; qualquer coisa maior e descartada */

/* -------------------------------------------------------------------------
 * Estado (consultado pela API REST — ver web_server.c)
 * ------------------------------------------------------------------------- */

static SemaphoreHandle_t s_status_mutex;
static slimproto_status_t s_status = {0};

/* IP (network order) do servidor de controle -- usado como fallback quando
 * o STRM recebido traz server_ip=0 ("use o mesmo servidor da conexao de
 * controle"), o caso mais comum na pratica. */
static uint32_t s_control_server_ip;

/* Socket da conexao de DADOS (separada da de controle) da sessao atual --
 * protegido por mutex porque a task de controle (ao receber um novo 's' ou
 * um 'q'/'f') pode fechar esse socket enquanto a task de dados ainda esta
 * bloqueada num recv() nele; e assim que sinalizamos "para" pra ela, sem
 * precisar (nem arriscar) chamar vTaskDelete de fora. */
static SemaphoreHandle_t s_data_mutex;
static int s_data_sock = -1;
static volatile bool s_data_paused = false;

typedef struct {
    uint32_t server_ip;   /* network order */
    uint16_t server_port;
    char http_header[MAX_CTRL_PAYLOAD];
    size_t http_header_len;
    char pcm_sample_size;
    char pcm_sample_rate;
    char pcm_channels;
    char pcm_endian;
} stream_start_args_t;

static void slimproto_data_task(void *arg);

/* -------------------------------------------------------------------------
 * Codificação/decodificação de campos binários (big-endian, ver protocolo
 * Slimproto/LMS: https://wiki.lyrion.org/index.php/SlimProto_TCP_protocol.html)
 * ------------------------------------------------------------------------- */

static void put_u8(uint8_t **p, uint8_t v) { *(*p)++ = v; }

static void put_u16(uint8_t **p, uint16_t v)
{
    uint16_t be = htons(v);
    memcpy(*p, &be, 2);
    *p += 2;
}

static void put_u32(uint8_t **p, uint32_t v)
{
    uint32_t be = htonl(v);
    memcpy(*p, &be, 4);
    *p += 4;
}

static void put_u64(uint8_t **p, uint64_t v)
{
    put_u32(p, (uint32_t)(v >> 32));
    put_u32(p, (uint32_t)(v & 0xffffffffu));
}

static uint16_t get_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return ntohs(v);
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return ntohl(v);
}

/* -------------------------------------------------------------------------
 * Envio de mensagens (opcode ASCII de 4 bytes + tamanho + payload)
 * ------------------------------------------------------------------------- */

static bool send_all(int sock, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool send_message(int sock, const char *opcode4, const uint8_t *payload, size_t payload_len)
{
    uint8_t header[8];
    memcpy(header, opcode4, 4);
    uint32_t len_be = htonl((uint32_t)payload_len);
    memcpy(header + 4, &len_be, 4);
    if (!send_all(sock, header, sizeof(header))) {
        return false;
    }
    return payload_len == 0 || send_all(sock, payload, payload_len);
}

static bool send_helo(int sock)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    /* So anunciamos "pcm" -- forca o servidor a sempre mandar PCM cru (sem
     * FLAC/MP3/etc), que e o que audio_codec_write() consome direto, sem
     * decoder nenhum no ESP32. */
    static const char capabilities[] =
        "Model=ReceiverBT,ModelName=ReceiverBT,AccuratePlayPoints=0,"
        "HasDigitalOut=0,HasPolarityInversion=0,MaxSampleRate=48000,pcm";

    uint8_t payload[1 + 1 + 6 + 16 + 2 + 8 + 2 + sizeof(capabilities)];
    uint8_t *p = payload;
    put_u8(&p, 12); /* deviceid: "squeezeplay" -- generico, cliente de software */
    put_u8(&p, 0);  /* revision */
    memcpy(p, mac, 6);
    p += 6;
    memset(p, 0, 16); /* uuid -- nao usado */
    p += 16;
    put_u16(&p, 0); /* wlan_channellist */
    put_u64(&p, 0); /* bytes_received */
    memcpy(p, "en", 2);
    p += 2;
    size_t cap_len = strlen(capabilities);
    memcpy(p, capabilities, cap_len);
    p += cap_len;

    return send_message(sock, "HELO", payload, (size_t)(p - payload));
}

/* server_timestamp: só é relevante em resposta a um 'strm t' (ecoa de volta
 * o campo replay_gain recebido) -- 0 nos demais eventos. Os campos de
 * buffer/posição são só informativos pro servidor/UI; não rastreamos com
 * precisão nesta primeira versão (não afeta a reprodução em si). */
static bool send_stat(int sock, const char *event_code, uint32_t server_timestamp)
{
    uint8_t payload[53];
    uint8_t *p = payload;
    memcpy(p, event_code, 4);
    p += 4;
    put_u8(&p, 0); /* crlf count */
    put_u8(&p, 0); /* mas_initialized */
    put_u8(&p, 0); /* mas_mode */
    put_u32(&p, 8192); /* stream_buffer_size (informativo) */
    put_u32(&p, 0);    /* stream_buffer_fullness */
    put_u64(&p, 0);    /* bytes_received */
    put_u16(&p, 0xffff); /* signal_strength: N/A (equivalente a "com fio") */
    put_u32(&p, (uint32_t)(esp_timer_get_time() / 1000)); /* jiffies (ms desde boot) */
    put_u32(&p, 8192); /* output_buffer_size */
    put_u32(&p, 0);    /* output_buffer_fullness */
    put_u32(&p, 0);    /* elapsed_seconds */
    put_u16(&p, 0);    /* voltage */
    put_u32(&p, 0);    /* elapsed_milliseconds */
    put_u32(&p, server_timestamp);
    put_u16(&p, 0); /* error_code */

    return send_message(sock, "STAT", payload, (size_t)(p - payload));
}

/* -------------------------------------------------------------------------
 * Sessão de dados (conexão HTTP separada, aberta a pedido de um 'strm s')
 * ------------------------------------------------------------------------- */

static void stop_stream_session(void)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (s_data_sock >= 0) {
        shutdown(s_data_sock, SHUT_RDWR);
        close(s_data_sock);
        s_data_sock = -1;
    }
    xSemaphoreGive(s_data_mutex);
    /* Não chamamos vTaskDelete aqui de propósito -- a task de dados percebe
     * o socket fechado (recv retorna erro) e se autodeleta sozinha. Isso
     * evita qualquer corrida de double-delete entre "quem manda parar" e
     * "quem termina por conta própria" (fim normal do stream). */
}

static const int PCM_RATE_TABLE[10] = {
    11025, 22050, 32000, 44100, 48000, 8000, 12000, 16000, 24000, 96000,
};

static int pcm_rate_to_hz(char c)
{
    if (c >= '0' && c <= '9') {
        return PCM_RATE_TABLE[c - '0'];
    }
    return 44100; /* '?' = auto-descritivo (cabeçalho WAV) -- não suportado ainda, usa um default razoável */
}

static void start_stream_session(uint32_t server_ip, uint16_t server_port,
                                  const uint8_t *header, size_t header_len,
                                  char pcm_size, char pcm_rate, char pcm_channels, char pcm_endian)
{
    stop_stream_session();

    stream_start_args_t *args = malloc(sizeof(*args));
    if (args == NULL) {
        ESP_LOGE(TAG, "sem heap para iniciar sessao de audio");
        return;
    }
    args->server_ip = server_ip != 0 ? server_ip : s_control_server_ip;
    args->server_port = server_port != 0 ? server_port : SLIMPROTO_SERVER_PORT;
    size_t copy_len = header_len < sizeof(args->http_header) ? header_len : sizeof(args->http_header);
    memcpy(args->http_header, header, copy_len);
    args->http_header_len = copy_len;
    args->pcm_sample_size = pcm_size;
    args->pcm_sample_rate = pcm_rate;
    args->pcm_channels = pcm_channels;
    args->pcm_endian = pcm_endian;

    if (pcm_size == '?' || pcm_rate == '?' || pcm_channels == '?') {
        ESP_LOGW(TAG, "PCM auto-descritivo (WAV) recebido -- ainda nao suportado, tentando 44.1kHz/16-bit/estereo");
    }

    s_data_paused = false;

    /* Pequena folga pra task de dados da sessao anterior (se houver)
     * perceber o socket fechado por stop_stream_session() e se autodeletar
     * antes de outra comecar a escrever no mesmo codec de audio. */
    vTaskDelay(pdMS_TO_TICKS(50));

    if (xTaskCreate(slimproto_data_task, "slim_data", 4096, args, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "falha ao criar task de dados (heap insuficiente)");
        free(args);
    }
}

static void slimproto_data_task(void *arg)
{
    stream_start_args_t *args = (stream_start_args_t *)arg;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(args->server_port);
    addr.sin_addr.s_addr = args->server_ip;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "falha ao criar socket de dados");
        goto cleanup_no_sock;
    }

    {
        struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "falha ao conectar no socket de dados (porta %d)", ntohs(addr.sin_port));
        goto cleanup;
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    s_data_sock = sock;
    xSemaphoreGive(s_data_mutex);

    if (!send_all(sock, (const uint8_t *)args->http_header, args->http_header_len)) {
        ESP_LOGW(TAG, "falha ao enviar cabecalho HTTP pro socket de dados");
        goto cleanup;
    }

    /* Consome a resposta HTTP (status + headers) até a linha em branco --
     * o que vem depois é PCM cru, sem mais nenhum envelope. */
    {
        int crlf_run = 0;
        while (crlf_run < 4) {
            char c;
            int n = recv(sock, &c, 1, 0);
            if (n <= 0) {
                ESP_LOGW(TAG, "conexao de dados fechada durante o cabecalho HTTP");
                goto cleanup;
            }
            crlf_run = (c == '\r' || c == '\n') ? crlf_run + 1 : 0;
        }
    }

    {
        int sample_rate = pcm_rate_to_hz(args->pcm_sample_rate);
        bool mono = (args->pcm_channels == '1');
        bool big_endian = (args->pcm_endian == '0');

        audio_codec_reconfigure_clock((uint32_t)sample_rate);
        logger_log(ESP_LOG_INFO, TAG, "Slimproto: stream iniciado (%d Hz, %s, %s-endian)",
                   sample_rate, mono ? "mono" : "estereo", big_endian ? "big" : "little");

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.playing = true;
        xSemaphoreGive(s_status_mutex);
        relay_control_notify_playing(true);
        audio_codec_set_mute(false);

        uint8_t buf[512];
        uint8_t out[1024];
        for (;;) {
            if (s_data_paused) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            int n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                break; /* servidor fechou (fim da faixa) ou erro/timeout */
            }
            size_t n_even = (size_t)n & ~((size_t)1); /* amostras de 16 bits -- ignora byte impar sobrando */

            /* BT sempre tem prioridade sobre o Slimproto: se estiver
             * conectado, descartamos o audio em vez de brigar pelo mesmo
             * I2S. Ver slimproto.h. */
            bt_audio_status_t bt;
            bt_audio_get_status(&bt);
            if (bt.connected) {
                continue;
            }

            if (big_endian) {
                for (size_t i = 0; i + 1 < n_even; i += 2) {
                    uint8_t tmp = buf[i];
                    buf[i] = buf[i + 1];
                    buf[i + 1] = tmp;
                }
            }

            const uint8_t *out_ptr = buf;
            size_t out_len = n_even;
            if (mono) {
                size_t frames = n_even / 2;
                if (frames > sizeof(out) / 4) {
                    frames = sizeof(out) / 4;
                }
                for (size_t i = 0; i < frames; i++) {
                    out[i * 4 + 0] = buf[i * 2 + 0];
                    out[i * 4 + 1] = buf[i * 2 + 1];
                    out[i * 4 + 2] = buf[i * 2 + 0];
                    out[i * 4 + 3] = buf[i * 2 + 1];
                }
                out_ptr = out;
                out_len = frames * 4;
            }

            size_t written = 0;
            audio_codec_write(out_ptr, out_len, &written);
        }
    }

cleanup:
    audio_codec_set_mute(true);
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (s_data_sock == sock) {
        s_data_sock = -1;
    }
    xSemaphoreGive(s_data_mutex);
    close(sock);
cleanup_no_sock:
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.playing = false;
    xSemaphoreGive(s_status_mutex);
    relay_control_notify_playing(false);
    free(args);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Comandos recebidos na conexão de controle
 * ------------------------------------------------------------------------- */

static void handle_strm(int ctrl_sock, const uint8_t *payload, size_t len)
{
    if (len < 24) {
        ESP_LOGW(TAG, "strm curto demais (%u bytes)", (unsigned)len);
        return;
    }

    char command = (char)payload[0];
    char format = (char)payload[2];
    char pcm_size = (char)payload[3];
    char pcm_rate = (char)payload[4];
    char pcm_channels = (char)payload[5];
    char pcm_endian = (char)payload[6];
    uint16_t server_port = get_u16(payload + 18);
    uint32_t server_ip = get_u32(payload + 20);

    switch (command) {
        case 's': /* start */
            if (format != 'p') {
                ESP_LOGW(TAG, "formato '%c' nao suportado (so PCM), ignorando faixa", format);
                send_stat(ctrl_sock, "STMn", 0);
                break;
            }
            start_stream_session(htonl(server_ip), server_port, payload + 24, len - 24,
                                  pcm_size, pcm_rate, pcm_channels, pcm_endian);
            break;
        case 'p': /* pause */
            s_data_paused = true;
            audio_codec_set_mute(true);
            break;
        case 'u': /* unpause */
            s_data_paused = false;
            audio_codec_set_mute(false);
            break;
        case 'q': /* stop */
        case 'f': /* flush */
            stop_stream_session();
            send_stat(ctrl_sock, "STMf", 0);
            break;
        case 't': { /* status/time request -- ecoa o timestamp do servidor (campo replay_gain) */
            uint32_t ts = get_u32(payload + 14);
            send_stat(ctrl_sock, "STMt", ts);
            break;
        }
        default:
            break;
    }
}

/* O servidor pergunta o nome do player (id 0x00) logo depois do HELO --
 * sem responder, ele usa um nome generico ("squeezeplay: <mac>"). Outros
 * ids (ex.: 0xfe, nao documentado) sao so ignorados, como o squeezelite
 * tambem faz. */
static void handle_setd(int sock, const uint8_t *payload, size_t len)
{
    if (len < 1 || payload[0] != 0x00) {
        return;
    }

    char device_name[32];
    if (storage_get_str(NVS_KEY_DEVICE_NAME, device_name, sizeof(device_name)) != ESP_OK || device_name[0] == '\0') {
        strlcpy(device_name, FW_DEVICE_NAME_DEFAULT, sizeof(device_name));
    }

    uint8_t resp[1 + sizeof(device_name)];
    resp[0] = 0x00;
    size_t name_len = strlen(device_name);
    memcpy(resp + 1, device_name, name_len);
    resp[1 + name_len] = '\0';
    send_message(sock, "setd", resp, 1 + name_len + 1);
}

/* -------------------------------------------------------------------------
 * Task de controle: conecta, manda HELO, processa comandos e manda
 * heartbeat (STAT/STMt) periodico. Reconecta sozinha se cair.
 * ------------------------------------------------------------------------- */

static bool recv_exact(int sock, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = recv(sock, buf + got, len - got, 0);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static void slimproto_control_task(void *arg)
{
    for (;;) {
        char host[64];
        bool has_host = (storage_get_str(NVS_KEY_SLIM_HOST, host, sizeof(host)) == ESP_OK) && host[0] != '\0';

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.enabled = has_host;
        strlcpy(s_status.server_host, has_host ? host : "", sizeof(s_status.server_host));
        xSemaphoreGive(s_status_mutex);

        if (!has_host) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        char ip_str[16];
        wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
        if (ip_str[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
        struct addrinfo *res = NULL;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%d", SLIMPROTO_SERVER_PORT);
        if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
            ESP_LOGW(TAG, "nao foi possivel resolver '%s'", host);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) {
            freeaddrinfo(res);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        s_control_server_ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;

        bool ok = (connect(sock, res->ai_addr, res->ai_addrlen) == 0);
        freeaddrinfo(res);

        if (!ok) {
            ESP_LOGW(TAG, "falha ao conectar em %s:%d", host, SLIMPROTO_SERVER_PORT);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        if (!send_helo(sock)) {
            ESP_LOGW(TAG, "falha ao enviar HELO");
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        logger_log(ESP_LOG_INFO, TAG, "Slimproto conectado em %s:%d", host, SLIMPROTO_SERVER_PORT);
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.connected = true;
        xSemaphoreGive(s_status_mutex);

        send_stat(sock, "STMt", 0);
        int64_t last_heartbeat = esp_timer_get_time();

        bool alive = true;
        while (alive) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            struct timeval sel_tv = {.tv_sec = 1, .tv_usec = 0};
            int r = select(sock + 1, &rfds, NULL, NULL, &sel_tv);

            if (r < 0) {
                break;
            }

            if (r > 0 && FD_ISSET(sock, &rfds)) {
                /* Assimetria real do protocolo (confirmada capturando bytes
                 * crus de um servidor de verdade, nao só pela doc): mensagens
                 * CLIENTE->SERVOR (HELO/STAT, ver send_message) usam opcode de
                 * 4 bytes + tamanho de 4 bytes: mensagens SERVIDOR->CLIENTE
                 * (strm/vers/setd/aude/audg/...) usam so 2 bytes de tamanho
                 * (cobrindo opcode+payload juntos) NA FRENTE do opcode. Usar o
                 * framing de 8 bytes pros dois lados (erro original) lia 2
                 * bytes a mais do que devia logo na primeira mensagem, e
                 * desincronizava tudo dali pra frente. */
                uint8_t len_hdr[2];
                if (!recv_exact(sock, len_hdr, sizeof(len_hdr))) {
                    break;
                }
                uint16_t total_len = get_u16(len_hdr); /* = 4 (opcode) + tamanho do payload */
                if (total_len < 4 || total_len > 4 + MAX_CTRL_PAYLOAD) {
                    ESP_LOGW(TAG, "mensagem com tamanho invalido (%u), desconectando", (unsigned)total_len);
                    break;
                }
                uint8_t msg[4 + MAX_CTRL_PAYLOAD];
                if (!recv_exact(sock, msg, total_len)) {
                    break;
                }
                size_t plen = total_len - 4;
                if (memcmp(msg, "strm", 4) == 0) {
                    handle_strm(sock, msg + 4, plen);
                } else if (memcmp(msg, "setd", 4) == 0) {
                    handle_setd(sock, msg + 4, plen);
                }
                /* qualquer outro opcode (aude/audg/vers/...) so e descartado
                 * -- ja consumimos exatamente `total_len` bytes acima, entao
                 * o framing da conexao continua sincronizado. */
            }

            int64_t now = esp_timer_get_time();
            if (now - last_heartbeat > HEARTBEAT_INTERVAL_US) {
                if (!send_stat(sock, "STMt", 0)) {
                    break;
                }
                last_heartbeat = now;
            }
        }

        logger_log(ESP_LOG_WARN, TAG, "Slimproto: conexao perdida, tentando de novo");
        stop_stream_session();
        close(sock);
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.connected = false;
        xSemaphoreGive(s_status_mutex);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

/* -------------------------------------------------------------------------
 * API pública
 * ------------------------------------------------------------------------- */

void slimproto_get_status(slimproto_status_t *out)
{
    if (out == NULL || s_status_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mutex);
}

void slimproto_init(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    s_data_mutex = xSemaphoreCreateMutex();
    s_data_sock = -1;

    xTaskCreate(slimproto_control_task, "slim_ctrl", 4096, NULL, 4, NULL);
}
