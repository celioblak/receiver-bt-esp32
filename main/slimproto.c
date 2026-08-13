#include "slimproto.h"

#include <ctype.h>
#include <errno.h>
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
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "slimproto";

#define HEARTBEAT_INTERVAL_US   (5000 * 1000)
#define RECONNECT_DELAY_MS      5000
#define MAX_CTRL_PAYLOAD        768   /* strm (24 + cabecalho HTTP) cabe folgado; qualquer coisa maior e descartada */

/* -------------------------------------------------------------------------
 * Buffer circular + task dedicada de I2S -- mesmo padrao do bt_audio.c.
 * Sem isso, a task de dados escrevia direto no codec logo apos cada
 * recv(), sem nenhuma folga contra instabilidade de rede -- qualquer
 * engasgo/atraso na entrega dos pacotes (confirmado: a mesma instabilidade
 * de WiFi que ja atrapalhava o resto da sessao) virava um engasgo audivel
 * na hora, sem chance de absorver. Com PSRAM sobrando (~4MB livres), um
 * buffer generoso de 64KB (~0.4s de audio 44.1kHz/16-bit estereo) e barato
 * e da folga real. Pre-alocado uma vez em slimproto_init() e fica vivo a
 * vida toda do dispositivo (a task so bloqueia no semaforo quando ocioso,
 * custo de CPU ~zero entre faixas).
 * ------------------------------------------------------------------------- */

/* Aumentado de 64/40KB para 128/80KB (2026-08-12): o gap observado em
 * alguns pulos de faixa (nem todos -- inconsistente, ver
 * project_slimproto_music_assistant memory) bate com a instabilidade de
 * WiFi ja conhecida ([[project_wifi_instability_unresolved]]) -- a troca de
 * faixa abre conexao TCP nova + pede o cabecalho HTTP de novo, exatamente a
 * operacao mais vulneravel a um soluco de rede. Mais folga aqui (dados vem
 * de PSRAM, ~4MB livres -- nao consome a RAM interna escassa que WiFi/BT
 * precisam) da mais margem sem custar nada de RAM interna; so alonga o
 * silencio antes de cada faixa comecar em ~200ms (ainda bem abaixo de
 * perceptivel como "travado"). */
#define SLIM_RINGBUF_HIGHEST_WATER_LEVEL  (128 * 1024)
#define SLIM_RINGBUF_PREFETCH_WATER_LEVEL (80 * 1024)

typedef enum {
    SLIM_RINGBUF_MODE_PROCESSING,
    SLIM_RINGBUF_MODE_PREFETCHING,
} slim_ringbuf_mode_t;

static RingbufHandle_t s_slim_ringbuf = NULL;
static SemaphoreHandle_t s_slim_i2s_sem = NULL;
/* Comeca em PREFETCHING (nao PROCESSING): a task escritora fica bloqueada no
 * semaforo ate o buffer atingir o watermark de prefetch pela primeira vez --
 * se comecasse em PROCESSING, nada jamais chamaria xSemaphoreGive() (isso so
 * acontece na transicao PREFETCHING->PROCESSING) e a task dormiria pra
 * sempre. */
static slim_ringbuf_mode_t s_slim_ringbuf_mode = SLIM_RINGBUF_MODE_PREFETCHING;

/* Incrementado a cada stop_stream_session() (troca/pulo de faixa OU stop
 * puro). Existe porque fechar o socket de dados NAO interrompe uma task
 * antiga que esteja bloqueada dentro de slim_write_ringbuf() esperando
 * espaco no ring buffer (o backpressure do buffer bloqueia por ate 3s,
 * sem relacao nenhuma com o socket) -- sem isso, um pedaco de audio da
 * faixa ANTERIOR podia "vazar" pro buffer bem depois do flush() da faixa
 * nova ja ter rodado, embaralhando o inicio da faixa nova (o engasgo real
 * ao pular de faixa). Cada slim_write_ringbuf() confere se ainda pertence
 * a sessao atual antes (e durante) a espera; se nao pertence mais,
 * descarta em vez de escrever. */
static volatile uint32_t s_slim_session_id = 0;

/* IMPORTANTE: envio aqui e BLOQUEANTE (nao "tenta e descarta" como no
 * bt_audio.c). Diferenca fundamental do transporte: o A2DP do Bluetooth ja
 * entrega audio no ritmo real de reproducao, entao um buffer cheio la e
 * raro/anormal. Ja o Music Assistant manda o arquivo inteiro via HTTP/TCP o
 * mais rapido que a rede permitir, sem se importar com o ritmo de playback
 * -- descartar pacotes quando o buffer enche (como a primeira versao deste
 * codigo fazia) virava o estado NORMAL de operacao, nao uma excecao, porque
 * a rede sempre entrega mais rapido do que os 176KB/s que o I2S consome.
 * Resultado pratico (confirmado pelo usuario): audio picotado E acelerado,
 * porque trechos inteiros de audio eram jogados fora continuamente.
 * Bloquear aqui aplica backpressure de verdade: a task de dados do
 * slimproto (que chama esta funcao) fica presa, para de chamar recv(), o
 * buffer de recepcao do socket TCP enche, e o proprio TCP freia o servidor
 * automaticamente -- exatamente o mecanismo que a versao anterior (escrita
 * direta e bloqueante no codec) tinha de graca e que a versao "buffered com
 * descarte" tinha quebrado. */
static size_t slim_write_ringbuf(uint32_t session_id, const uint8_t *data, size_t size)
{
    if (s_slim_ringbuf == NULL || size == 0) {
        return 0;
    }

    /* Espera em fatias curtas (nao um unico xRingbufferSend de 3s) e confere
     * a sessao a cada volta -- se a faixa foi trocada/parada enquanto esta
     * chamada estava bloqueada esperando espaco, aborta e descarta em vez de
     * escrever um pedaco atrasado da faixa ANTERIOR depois que a faixa nova
     * ja comecou (era exatamente isso que causava o engasgo ao pular de
     * faixa mesmo com o flush() no inicio da task nova: fechar o socket nao
     * interrompe quem ja estava bloqueado aqui dentro). */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    for (;;) {
        if (session_id != s_slim_session_id) {
            logger_log(ESP_LOG_WARN, TAG, "slim_write_ringbuf: descartando (sessao %u, atual %u)",
                       (unsigned)session_id, (unsigned)s_slim_session_id);
            return 0; /* sessao cancelada -- descarta silenciosamente */
        }
        BaseType_t done = xRingbufferSend(s_slim_ringbuf, (void *)data, size, pdMS_TO_TICKS(100));
        if (done) {
            break;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            logger_log(ESP_LOG_ERROR, TAG, "ring buffer de audio slimproto nao drenou em 3s (sessao %u), descartando pacote",
                       (unsigned)session_id);
            return 0;
        }
        /* ainda dentro do prazo total -- tenta de novo (proxima volta confere a sessao) */
    }

    if (s_slim_ringbuf_mode == SLIM_RINGBUF_MODE_PREFETCHING) {
        size_t used = 0;
        vRingbufferGetInfo(s_slim_ringbuf, NULL, NULL, NULL, NULL, &used);
        if (used >= SLIM_RINGBUF_PREFETCH_WATER_LEVEL) {
            s_slim_ringbuf_mode = SLIM_RINGBUF_MODE_PROCESSING;
            xSemaphoreGive(s_slim_i2s_sem);
        }
    }
    return size;
}

/* Chamada no inicio de cada nova faixa (slimproto_data_task), antes de
 * reconfigurar o clock do I2S ou empurrar qualquer byte da faixa nova.
 * Sem isso, sobras de audio da faixa ANTERIOR (ate ~360ms, o tamanho do
 * buffer) ainda paradas no ring buffer viravam vitima do
 * audio_codec_reconfigure_clock() da faixa nova (desabilita/reabilita o
 * canal I2S no meio do caminho) -- e exatamente o cenario de troca/pulo de
 * faixa que causava o engasgo relatado mesmo depois do buffer resolver o
 * picotamento durante uma musica so. */
static void slim_ringbuf_flush(void)
{
    if (s_slim_ringbuf == NULL) {
        return;
    }
    for (;;) {
        size_t item_size = 0;
        void *data = xRingbufferReceiveUpTo(s_slim_ringbuf, &item_size, 0, SLIM_RINGBUF_HIGHEST_WATER_LEVEL);
        if (item_size == 0) {
            break;
        }
        vRingbufferReturnItem(s_slim_ringbuf, data);
    }
    s_slim_ringbuf_mode = SLIM_RINGBUF_MODE_PREFETCHING;
}

static void slim_i2s_task_handler(void *arg)
{
    const size_t max_chunk = 240 * 6;

    for (;;) {
        if (xSemaphoreTake(s_slim_i2s_sem, portMAX_DELAY) == pdTRUE) {
            for (;;) {
                size_t item_size = 0;
                uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
                    s_slim_ringbuf, &item_size, pdMS_TO_TICKS(20), max_chunk);
                if (item_size == 0) {
                    s_slim_ringbuf_mode = SLIM_RINGBUF_MODE_PREFETCHING;
                    break;
                }
                size_t written = 0;
                audio_codec_write(data, item_size, &written);
                vRingbufferReturnItem(s_slim_ringbuf, data);
            }
        }
    }
}

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
    uint32_t session_id;
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
     * decoder nenhum no ESP32.
     *
     * Campos (Model=squeezelite, AccuratePlayPoints=1, HasDigitalOut=1,
     * HasPolarityInversion=1, Firmware=) copiados EXATAMENTE do BASE_CAP do
     * squeezelite-esp32 real (que funciona com o MA), em vez dos valores
     * "genericos" que tinhamos antes -- suspeita (nao 100% confirmada, o
     * codigo do provider do MA que decide isso nao ficou totalmente claro
     * nem investigando o source) de que o MA trata "Model=squeezelite" como
     * cliente conhecido/confiavel pra negociacao de codec, e um Model
     * desconhecido cai num fallback mais conservador (FLAC) ignorando o
     * "pcm" que a gente ja mandava certo na lista de codecs. */
    /* "pcm,flc": pcm primeiro (preferencia), flc so pra nao disparar o aviso
     * "Player did not report support for content_type audio/flac" do
     * aioslimproto -- confirmado via log de debug do MA que esse aviso e
     * seguido de silencio total (nenhum STRM chega no dispositivo depois
     * dele). NAO decodificamos FLAC de verdade; a esperanca e que o MA
     * escolha PCM por estar primeiro na lista quando os dois batem. Se o
     * servidor mandar formato 'f' mesmo assim, handle_strm() ja rejeita e
     * loga, sem travar nada -- so nao vai tocar essa faixa especificamente. */
    static const char capabilities[] =
        "Model=squeezelite,AccuratePlayPoints=1,HasDigitalOut=1,"
        "HasPolarityInversion=1,Firmware=" FW_VERSION
        ",ModelName=ReceiverBT,MaxSampleRate=48000,pcm,flc";

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
    uint32_t old_id = s_slim_session_id;
    s_slim_session_id++; /* invalida qualquer write bloqueado da sessao anterior -- ver slim_write_ringbuf() */
    logger_log(ESP_LOG_INFO, TAG, "stop_stream_session: sessao %u -> %u (sock=%d)",
               (unsigned)old_id, (unsigned)s_slim_session_id, s_data_sock);
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

/* '0'=8-bit, '1'=16-bit, '2'=20/24-bit (empacotado em 3 bytes na pratica),
 * '3'=32-bit. Precisamos disso porque o Music Assistant manda WAV em
 * 24-bit por padrao (so descobrimos tocando de verdade -- deu um chiado
 * constante, amostras de 16-bit sendo lidas erradas a partir de dado de
 * 24-bit). audio_codec_write() so aceita 16-bit, entao convertemos aqui. */
static size_t pcm_bytes_per_sample(char c)
{
    switch (c) {
        case '0': return 1;
        case '2': return 3;
        case '3': return 4;
        default: return 2; /* '1' e qualquer coisa inesperada -- 16-bit e o caso comum */
    }
}

/* Reduz uma amostra de N bytes pra 16 bits pegando os bytes mais
 * significativos (truncamento simples, sem dither -- suficiente aqui,
 * a perda de precisao e inaudivel nesse uso). */
static int16_t pcm_extract_16bit(const uint8_t *p, size_t bytes_per_sample, bool big_endian)
{
    if (bytes_per_sample == 1) {
        /* 8-bit PCM e convencionalmente sem sinal (0-255, meio-de-escala=128) */
        return (int16_t)(((int)p[0] - 128) << 8);
    }
    if (big_endian) {
        return (int16_t)((p[0] << 8) | p[1]);
    }
    /* little-endian: os bytes mais significativos ficam no final da amostra */
    return (int16_t)((p[bytes_per_sample - 1] << 8) | p[bytes_per_sample - 2]);
}

/* Comparacao de substring sem diferenciar maiusculas/minusculas -- so pra
 * achar "chunked" dentro do texto cru dos headers HTTP. */
static bool contains_ci(const char *haystack, const char *needle)
{
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn == 0 || nn > hn) {
        return false;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for (; j < nn; j++) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) {
                break;
            }
        }
        if (j == nn) {
            return true;
        }
    }
    return false;
}

/* O corpo da resposta HTTP pode vir com "Transfer-Encoding: chunked"
 * (esperado pra um stream gerado ao vivo, de tamanho desconhecido -- e
 * exatamente o caso do Music Assistant transcodificando em tempo real).
 * Sem desembrulhar isso, os marcadores de tamanho de cada chunk (texto
 * ASCII em hexadecimal + CRLF, intercalados com os dados de verdade) eram
 * lidos como se fossem amostras de audio -- causa real de um chiado
 * constante E do audio ficando mais lento (amostras falsas empurrando o
 * conteudo real pra "depois" no tempo). */
typedef struct {
    bool chunked;
    uint32_t chunk_remaining; /* bytes que ainda faltam do chunk atual (so dados, sem contar o CRLF final) */
    bool need_chunk_size;     /* true = a proxima coisa a ler e uma linha "tamanho\r\n" */
} http_body_reader_t;

/* Le ate max_len bytes de conteudo JA desembrulhado (se chunked). Mesma
 * convencao de retorno do recv(): >0 bytes lidos, 0 = fim do corpo
 * (ultimo chunk "0\r\n" visto), <0 = erro (com errno setado pelo recv()
 * interno). */
static int http_body_read(int sock, http_body_reader_t *r, uint8_t *buf, size_t max_len)
{
    if (!r->chunked) {
        return recv(sock, buf, max_len, 0);
    }
    for (;;) {
        if (r->need_chunk_size) {
            char line[16];
            size_t len = 0;
            for (;;) {
                char c;
                int n = recv(sock, &c, 1, 0);
                if (n <= 0) {
                    return n;
                }
                if (c == '\n') {
                    break;
                }
                if (c != '\r' && len < sizeof(line) - 1) {
                    line[len++] = c;
                }
            }
            line[len] = '\0';
            for (size_t i = 0; i < len; i++) {
                if (line[i] == ';') { /* extensao de chunk (raro) -- ignora */
                    line[i] = '\0';
                    break;
                }
            }
            uint32_t size = (uint32_t)strtoul(line, NULL, 16);
            if (size == 0) {
                return 0; /* chunk final -- fim do corpo */
            }
            r->chunk_remaining = size;
            r->need_chunk_size = false;
        }
        if (r->chunk_remaining == 0) {
            /* acabou o chunk anterior -- engole o CRLF final dele antes do proximo tamanho */
            char crlf[2];
            int n = recv(sock, crlf, sizeof(crlf), 0);
            if (n <= 0) {
                return n;
            }
            r->need_chunk_size = true;
            continue;
        }
        size_t want = max_len < r->chunk_remaining ? max_len : r->chunk_remaining;
        int n = recv(sock, buf, want, 0);
        if (n <= 0) {
            return n;
        }
        r->chunk_remaining -= (uint32_t)n;
        return n;
    }
}

/* Le exatamente len bytes (via http_body_read, entao ja desembrulhado se
 * chunked), bloqueando -- usado so pra cabecalhos pequenos (WAV), onde um
 * n<=0 qualquer e tratado como falha; nao acontece na pratica porque
 * cabecalho chega inteiro quase instantaneo. */
static bool body_read_n(int sock, http_body_reader_t *r, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = http_body_read(sock, r, buf + got, len - got);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
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
    args->session_id = s_slim_session_id; /* stop_stream_session() acima ja incrementou */

    if (pcm_size == '?' || pcm_rate == '?' || pcm_channels == '?') {
        /* "Auto-descritivo": o header STRM nao traz o formato real, so o
         * WAV que vem no corpo (RIFF/fmt). Isso ja e coberto -- o parser de
         * WAV em slimproto_data_task le o chunk 'fmt ' de verdade e
         * substitui taxa/canais/bits antes de qualquer conversao (ver o
         * bloco "fmt real do WAV" mais abaixo). O valor guardado aqui
         * (args->pcm_sample_size etc.) e so um palpite de partida, sem
         * efeito pratico no audio final. */
        logger_log(ESP_LOG_INFO, TAG, "PCM auto-descritivo (WAV) recebido -- formato real vira do cabecalho WAV no corpo");
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
    uint32_t my_session = args->session_id;

    logger_log(ESP_LOG_INFO, TAG, "slimproto_data_task: iniciando sessao %u", (unsigned)my_session);
    slim_ringbuf_flush(); /* descarta sobra da faixa anterior antes de tocar a nova */

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

    /* Consome a resposta HTTP (status + headers) até a linha em branco,
     * guardando o texto pra procurar "Transfer-Encoding: chunked" -- o
     * corpo pode vir em chunks (comum pra um stream gerado ao vivo, de
     * tamanho desconhecido, exatamente o caso aqui). */
    http_body_reader_t reader = {.chunked = false, .chunk_remaining = 0, .need_chunk_size = true};
    {
        char header_text[512];
        size_t header_len = 0;
        int crlf_run = 0;
        while (crlf_run < 4) {
            char c;
            int n = recv(sock, &c, 1, 0);
            if (n <= 0) {
                ESP_LOGW(TAG, "conexao de dados fechada durante o cabecalho HTTP");
                goto cleanup;
            }
            if (header_len < sizeof(header_text) - 1) {
                header_text[header_len++] = c;
            }
            crlf_run = (c == '\r' || c == '\n') ? crlf_run + 1 : 0;
        }
        header_text[header_len] = '\0';
        reader.chunked = contains_ci(header_text, "chunked");
        if (reader.chunked) {
            logger_log(ESP_LOG_INFO, TAG, "Slimproto: corpo HTTP chunked, desembrulhando");
        }
    }

    {
        int sample_rate = pcm_rate_to_hz(args->pcm_sample_rate);
        bool mono = (args->pcm_channels == '1');
        bool big_endian = (args->pcm_endian == '0');
        size_t bytes_per_sample_in = pcm_bytes_per_sample(args->pcm_sample_size);

        /* frame_buf guarda ate 256 bytes novos + a sobra (amostra
         * incompleta) do recv() anterior, quando frame_size_in nao divide
         * exatamente o tamanho lido (ex.: amostras de 3 bytes). out e
         * dimensionado pro pior caso (256 quadros de 1 byte, mono 8-bit). */
        uint8_t frame_buf[256 + 8];
        uint8_t out[256 * 4];
        size_t carry_len = 0;

        /* O Music Assistant manda WAV de verdade agora (nao PCM cru), com
         * cabecalho RIFF/WAVE + chunks (fmt, as vezes LIST/outros) antes do
         * chunk "data" -- e so DEPOIS dele que comeca audio de verdade. Sem
         * pular isso certinho, os bytes do cabecalho entram como se fossem
         * amostras, desalinhando TUDO dali pra frente (foi a causa real de
         * um chiado constante, cobrindo a faixa inteira, nao so um click
         * no comeco -- o cabecalho real tem chunks extras que não são
         * multiplos do tamanho do quadro, entao o desalinhamento nunca se
         * corrige sozinho). Se nao vier "RIFF" (servidor mandando PCM cru
         * de verdade), os 4 bytes lidos aqui SAO audio -- entram como sobra
         * pro loop principal processar, sem descartar nada. */
        {
            uint8_t magic[4];
            if (!body_read_n(sock, &reader, magic, sizeof(magic))) {
                ESP_LOGW(TAG, "conexao de dados fechada logo no inicio do audio");
                goto cleanup;
            }
            logger_log(ESP_LOG_INFO, TAG, "Slimproto: primeiros 4 bytes do corpo: %02x %02x %02x %02x ('%c%c%c%c')",
                       magic[0], magic[1], magic[2], magic[3],
                       isprint(magic[0]) ? magic[0] : '.', isprint(magic[1]) ? magic[1] : '.',
                       isprint(magic[2]) ? magic[2] : '.', isprint(magic[3]) ? magic[3] : '.');
            if (memcmp(magic, "RIFF", 4) == 0) {
                uint8_t skip8[8]; /* riff_size(4) + "WAVE"(4) */
                if (!body_read_n(sock, &reader, skip8, sizeof(skip8))) {
                    goto cleanup;
                }
                for (;;) {
                    uint8_t chunk_hdr[8]; /* chunk_id(4) + chunk_size(4, little-endian) */
                    if (!body_read_n(sock, &reader, chunk_hdr, sizeof(chunk_hdr))) {
                        goto cleanup;
                    }
                    uint32_t chunk_size = (uint32_t)chunk_hdr[4] | ((uint32_t)chunk_hdr[5] << 8) |
                                           ((uint32_t)chunk_hdr[6] << 16) | ((uint32_t)chunk_hdr[7] << 24);
                    logger_log(ESP_LOG_INFO, TAG, "Slimproto: chunk WAV '%c%c%c%c' tamanho=%u",
                               isprint(chunk_hdr[0]) ? chunk_hdr[0] : '.', isprint(chunk_hdr[1]) ? chunk_hdr[1] : '.',
                               isprint(chunk_hdr[2]) ? chunk_hdr[2] : '.', isprint(chunk_hdr[3]) ? chunk_hdr[3] : '.',
                               (unsigned)chunk_size);
                    if (memcmp(chunk_hdr, "data", 4) == 0) {
                        break; /* audio comeca logo em seguida */
                    }
                    if (memcmp(chunk_hdr, "fmt ", 4) == 0 && chunk_size >= 16) {
                        /* WAVEFORMATEX: wFormatTag(2) nChannels(2) nSamplesPerSec(4)
                         * nAvgBytesPerSec(4) nBlockAlign(2) wBitsPerSample(2) [...]
                         * -- o formato real do arquivo pode nao bater com o que o
                         * cabecalho Slimproto anunciou (confirmado na pratica: STRM
                         * dizia 16-bit, o fmt chunk de verdade dizia 24-bit -- causa
                         * real do chiado/lentidao, amostras de 3 bytes lidas como
                         * se fossem de 2). Preferimos sempre o que o proprio WAV diz. */
                        uint8_t fmt_data[16];
                        if (!body_read_n(sock, &reader, fmt_data, sizeof(fmt_data))) {
                            goto cleanup;
                        }
                        uint16_t real_channels = (uint16_t)(fmt_data[2] | (fmt_data[3] << 8));
                        uint32_t real_rate = (uint32_t)fmt_data[4] | ((uint32_t)fmt_data[5] << 8) |
                                              ((uint32_t)fmt_data[6] << 16) | ((uint32_t)fmt_data[7] << 24);
                        uint16_t real_bits = (uint16_t)(fmt_data[14] | (fmt_data[15] << 8));
                        logger_log(ESP_LOG_INFO, TAG, "Slimproto: fmt real do WAV: %u canal(is), %u Hz, %u bits",
                                   real_channels, (unsigned)real_rate, real_bits);
                        if (real_channels == 1 || real_channels == 2) {
                            mono = (real_channels == 1);
                        }
                        if (real_rate > 0) {
                            sample_rate = (int)real_rate;
                        }
                        if (real_bits == 8 || real_bits == 16 || real_bits == 24 || real_bits == 32) {
                            bytes_per_sample_in = real_bits / 8;
                        }
                        uint32_t remaining = (chunk_size - 16) + (chunk_size & 1);
                        uint8_t trash[64];
                        while (remaining > 0) {
                            size_t want = remaining < sizeof(trash) ? remaining : sizeof(trash);
                            if (!body_read_n(sock, &reader, trash, want)) {
                                goto cleanup;
                            }
                            remaining -= (uint32_t)want;
                        }
                        continue;
                    }
                    uint32_t to_skip = chunk_size + (chunk_size & 1); /* chunks tem padding pra tamanho par */
                    uint8_t trash[64];
                    while (to_skip > 0) {
                        size_t want = to_skip < sizeof(trash) ? to_skip : sizeof(trash);
                        if (!body_read_n(sock, &reader, trash, want)) {
                            goto cleanup;
                        }
                        to_skip -= (uint32_t)want;
                    }
                }
            } else {
                /* nao e WAV -- ja e PCM cru, os 4 bytes lidos sao audio de verdade */
                memcpy(frame_buf, magic, sizeof(magic));
                carry_len = sizeof(magic);
            }
        }

        /* Calculado so agora, depois do fmt chunk do WAV (se veio) poder ter
         * corrigido mono/bytes_per_sample_in em relacao ao que o cabecalho
         * Slimproto anunciou. */
        size_t channels_in = mono ? 1 : 2;
        size_t frame_size_in = bytes_per_sample_in * channels_in;

        audio_codec_reconfigure_clock((uint32_t)sample_rate);
        logger_log(ESP_LOG_INFO, TAG, "Slimproto: stream iniciado (%d Hz, %u bits, %s, %s-endian)",
                   sample_rate, (unsigned)(bytes_per_sample_in * 8), mono ? "mono" : "estereo",
                   big_endian ? "big" : "little");

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.playing = true;
        xSemaphoreGive(s_status_mutex);
        relay_control_notify_playing(true);
        audio_codec_set_mute(false);

        for (;;) {
            if (s_data_paused) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            int n = http_body_read(sock, &reader, frame_buf + carry_len, 256);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                    /* SO_RCVTIMEO estourou so porque o servidor teve um
                     * intervalo natural sem mandar dado (buffer/rede) --
                     * NAO e fim de faixa. Tratar isso como erro derrubava a
                     * sessao (mutava, desligava o rele, zerava o status de
                     * "tocando") no meio de reproducoes que continuavam
                     * normais do ponto de vista do servidor. */
                    continue;
                }
                break; /* erro de verdade (ex.: stop_stream_session fechou o socket) */
            }
            if (n == 0) {
                break; /* EOF limpo -- servidor fechou de proposito (fim da faixa) */
            }

            /* BT sempre tem prioridade sobre o Slimproto: se estiver
             * conectado, descartamos o audio em vez de brigar pelo mesmo
             * I2S. Ver slimproto.h. */
            bt_audio_status_t bt;
            bt_audio_get_status(&bt);
            if (bt.connected) {
                carry_len = 0; /* descarta tambem a sobra -- nao faz sentido remontar depois de um gap */
                continue;
            }

            size_t total = carry_len + (size_t)n;
            size_t frames = total / frame_size_in;
            if (frames > sizeof(out) / 4) {
                frames = sizeof(out) / 4; /* nao deveria acontecer com os tamanhos atuais, so protege o buffer */
            }
            size_t usable_bytes = frames * frame_size_in;

            for (size_t f = 0; f < frames; f++) {
                const uint8_t *left = frame_buf + f * frame_size_in;
                const uint8_t *right = mono ? left : left + bytes_per_sample_in;
                int16_t l = pcm_extract_16bit(left, bytes_per_sample_in, big_endian);
                int16_t r = pcm_extract_16bit(right, bytes_per_sample_in, big_endian);
                out[f * 4 + 0] = (uint8_t)(l & 0xff);
                out[f * 4 + 1] = (uint8_t)((l >> 8) & 0xff);
                out[f * 4 + 2] = (uint8_t)(r & 0xff);
                out[f * 4 + 3] = (uint8_t)((r >> 8) & 0xff);
            }

            slim_write_ringbuf(my_session, out, frames * 4);

            /* guarda o resto (quadro incompleto) pro proximo recv() completar */
            size_t new_carry_len = total - usable_bytes;
            if (new_carry_len > 0) {
                memmove(frame_buf, frame_buf + usable_bytes, new_carry_len);
            }
            carry_len = new_carry_len;
        }
    }

cleanup:
    /* So remuda se esta sessao ainda for a atual -- se uma faixa nova ja
     * comecou (skip rapido), a task dela ja desmutou o codec, e mutar aqui
     * incondicionalmente corta o comeco da faixa nova numa corrida sem
     * ordem garantida entre "task antiga terminando" e "task nova
     * comecando" (era exatamente esse race o "engasgo" residual ao pular
     * de faixa, mesmo depois do buffer e do vazamento de audio corrigidos). */
    if (my_session == s_slim_session_id) {
        audio_codec_set_mute(true);
    }
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (s_data_sock == sock) {
        s_data_sock = -1;
    }
    xSemaphoreGive(s_data_mutex);
    close(sock);
cleanup_no_sock:
    /* NAO mexemos em s_status.playing/rele aqui de proposito: o fim desta
     * conexao de dados especifica nao significa "parou de tocar" -- o MA
     * abre uma conexao de dados nova a cada trecho/faixa (STRM 's' de novo),
     * entao isso disparava a cada poucos segundos mesmo com audio continuo,
     * fazendo o status "tocando" piscar false toda hora (visto direto pelo
     * usuario: audio real continuava, /api/status quase nunca pegava true).
     * Quem decide "playing=false" de verdade agora e so um STOP/FLUSH
     * explicito (handle_strm) ou a conexao de CONTROLE cair de vez
     * (slimproto_control_task). */
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
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.playing = false;
            xSemaphoreGive(s_status_mutex);
            relay_control_notify_playing(false);
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

/* Ajuste de volume vindo do servidor (ex.: controle deslizante do Music
 * Assistant, ou o botao de mudo dele) -- confirmado: sem tratar esse
 * opcode, o volume/mudo do MA nao tinha NENHUM efeito no dispositivo,
 * porque handle_ctrl_message() descartava "audg" junto com todo opcode que
 * nao fosse strm/setd. Payload (18 bytes, ver squeezelite/slimproto real):
 * old_gainL(4) old_gainR(4) adjust(1) preamp(1) gainL(4) gainR(4), gains em
 * ponto fixo 16.16 (65536 = 0dB/ganho unitario). Usamos so o canal
 * esquerdo -- nosso volume e um unico valor pros dois canais. */
static void handle_audg(const uint8_t *payload, size_t len)
{
    if (len < 18) {
        return;
    }
    uint32_t gain_l = get_u32(payload + 10);
    float linear = (float)gain_l / 65536.0f;
    if (linear > 1.0f) {
        linear = 1.0f;
    } else if (linear < 0.0f) {
        linear = 0.0f;
    }
    int volume = (int)(linear * VOLUME_STEPS + 0.5f);
    audio_codec_set_volume(volume);
    logger_log(ESP_LOG_INFO, TAG, "Slimproto: audg recebido (ganho=%.3f) -> volume %d/%d",
               linear, volume, VOLUME_STEPS);
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
                } else if (memcmp(msg, "audg", 4) == 0) {
                    handle_audg(msg + 4, plen);
                }
                /* qualquer outro opcode (aude/vers/... -- aude controla
                 * canais de saida individuais de hardware multi-saida, nao
                 * se aplica aqui) so e descartado -- ja consumimos
                 * exatamente `total_len` bytes acima, entao o framing da
                 * conexao continua sincronizado. */
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
        bool was_playing = s_status.playing;
        s_status.playing = false;
        xSemaphoreGive(s_status_mutex);
        if (was_playing) {
            /* So mexe no rele se a gente REALMENTE estava tocando algo --
             * senao, cada reconexao (frequente com a rede instavel)
             * reiniciava o timer de desligamento do zero pra sempre,
             * mesmo sem nunca ter tocado nada (ex.: colidindo com o bipe
             * de teste do boot, que liga o rele por conta propria). */
            relay_control_notify_playing(false);
        }
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

    /* Pre-alocado uma unica vez, aqui no boot, junto com a task dedicada de
     * I2S -- mesmo motivo do bt_audio.c: evita competir por heap contra
     * WiFi/BT durante o meio de uma sessao de streaming. A task fica viva
     * a vida toda do dispositivo, bloqueada no semaforo quando ociosa. */
    s_slim_ringbuf = xRingbufferCreate(SLIM_RINGBUF_HIGHEST_WATER_LEVEL, RINGBUF_TYPE_BYTEBUF);
    s_slim_i2s_sem = xSemaphoreCreateBinary();
    xTaskCreate(slim_i2s_task_handler, "slim_i2s", 3072, NULL, 6, NULL);

    xTaskCreate(slimproto_control_task, "slim_ctrl", 4096, NULL, 4, NULL);
}
