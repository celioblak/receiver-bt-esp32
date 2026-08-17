#include "dlna_renderer.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

#include "audio_agc.h"
#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "flac_stream_decoder.h"
#include "logger.h"
#include "relay_control.h"
#include "storage.h"
#include "wifi_manager.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "dlna";

#define SSDP_PORT               1900
#define SSDP_MCAST_ADDR         "239.255.255.250"
#define SSDP_NOTIFY_INTERVAL_S  60
#define DLNA_HTTP_PORT          8200
#define DLNA_HTTP_CTRL_PORT     32769 /* precisa ser diferente do servidor principal (web_server.c, ver esp_http_server) */

static char s_uuid[48];
static char s_friendly_name[32];
static httpd_handle_t s_httpd = NULL;

/* Estado do AVTransport, protegido por s_state_mutex (varias tasks tocam
 * isso: handler SOAP na task do httpd, a task de busca/decode, e leituras
 * de fora via dlna_renderer_get_status()). */
static SemaphoreHandle_t s_state_mutex = NULL;
static char s_current_uri[256] = "";
static volatile bool s_playing = false;

/* Estado de transporte no vocabulario UPnP AVTransport de verdade (nao so
 * "tocando ou nao") -- PAUSED_PLAYBACK e STOPPED sao coisas diferentes pro
 * control point (ex.: Music Assistant so mostra o botao de play/pause
 * corretamente se distinguir os dois). Antes disso so existia s_playing
 * (bool), que tratava Pause e Stop como a mesma coisa. */
typedef enum {
    DLNA_STATE_STOPPED,
    DLNA_STATE_PLAYING,
    DLNA_STATE_PAUSED,
} dlna_transport_state_t;
static volatile dlna_transport_state_t s_transport_state = DLNA_STATE_STOPPED;

static char s_track[64] = "";
static char s_artist[64] = "";
static char s_album[64] = "";
/* Duracao da faixa atual, "HH:MM:SS" -- extraida do atributo duration="..."
 * do <res> dentro do DIDL-Lite de CurrentURIMetaData (Music Assistant manda
 * isso de verdade). "00:00:00" (convencao UPnP pra "desconhecida") se nao
 * vier ou nao tiver faixa carregada. */
static char s_track_duration[16] = "00:00:00";
/* Posicao de reproducao real, contada por esp_timer_get_time() -- nao um
 * campo "de mentira" fixo em 00:00:00 como antes. s_playback_elapsed_base_us
 * acumula o que ja tocou (inclusive antes de pausas anteriores DESSA MESMA
 * faixa); s_playback_start_us marca quando a contagem atual (desde o ultimo
 * Play/resume) comecou. Enquanto DLNA_STATE_PLAYING, decorrido =
 * base + (agora - start); pausado/parado, decorrido = base (congelado). */
static volatile int64_t s_playback_elapsed_base_us = 0;
static volatile int64_t s_playback_start_us = 0;

/* Proxima faixa da fila, enfileirada pelo control point via
 * SetNextAVTransportURI -- o mecanismo PADRAO do UPnP AVTransport pra
 * reproducao continua/gapless COM metadados corretos por faixa.
 *
 * CAUSA RAIZ do problema de metadado/posicao errados (achado lendo o fonte
 * real do MA): o Music Assistant TENTA usar isso (metodo enqueue_next_media
 * -> async_set_next_transport_uri) e, quando o renderer nao suporta, cai no
 * "flow mode" -- um stream de audio continuo com varias faixas coladas na
 * mesma conexao, onde ele nunca mais avisa nada sobre troca de faixa (o
 * proprio aviso no codigo dele diz: "Enqueuing the next track failed for
 * player %s - the player probably doesn't support this. Enable 'flow mode'
 * for this player"). Como este firmware nao declarava nem implementava
 * SetNextAVTransportURI, o MA era EMPURRADO pro flow mode -- e ali titulo,
 * artista, album e duracao congelavam na primeira faixa pra sempre.
 * Implementando a acao, cada faixa volta a chegar com o proprio DIDL-Lite. */
/* Buffers grandes de XML (respostas SOAP com DIDL-Lite e corpo dos eventos)
 * ficam na PSRAM, NAO em RAM interna. Motivo concreto: com WiFi+BT+MQTT+dois
 * servidores HTTP rodando, o heap interno livre gira em ~20KB (visto no log
 * periodico do main.c) -- os ~20KB desses buffers como estaticos deixariam
 * quase nada e derrubariam alocacoes do WiFi/HTTP. Mesma decisao ja tomada
 * pro ring buffer de audio. Alocados uma vez no boot; se falhar, o DLNA nem
 * sobe (ver dlna_renderer_init). */
#define DLNA_BUF_SOAP_RESP   4200
#define DLNA_BUF_SOAP_BODY   3600
#define DLNA_BUF_DIDL_ESC    2400
#define DLNA_BUF_DIDL_RAW    1700
#define DLNA_BUF_EVT_INNER   1024
#define DLNA_BUF_EVT_ESC     1400
#define DLNA_BUF_EVT_BODY    1800
static char *s_soap_resp;    /* resposta SOAP montada (task do httpd) */
static char *s_soap_body;    /* corpo de GetPositionInfo/GetMediaInfo (httpd) */
static char *s_didl_esc;     /* DIDL-Lite da faixa atual, ja escapado (httpd) */
static char *s_didl_raw;     /* DIDL-Lite cru antes de escapar (httpd) */
static char *s_evt_inner;    /* XML do Event (task de eventing) */
static char *s_evt_esc;      /* Event escapado (task de eventing) */
static char *s_evt_body;     /* propertyset final (task de eventing) */
static bool s_xml_bufs_ok;

static char s_next_uri[256] = "";
static char s_next_track[64] = "";
static char s_next_artist[64] = "";
static char s_next_album[64] = "";
static char s_next_duration[16] = "00:00:00";
/* IP do ultimo control point que mandou uma acao SOAP de AVTransport (ex.:
 * Music Assistant) -- so pra exibir "quem esta conectado" na pagina web/MQTT,
 * capturado direto do socket TCP da requisicao (nao depende de nenhum
 * cabecalho que o control point possa nao mandar). */
static char s_client_ip[16] = "";
/* Cabecalho User-Agent da mesma requisicao -- costuma identificar a
 * biblioteca/app do control point (ex.: async-upnp-client usado pelo Music
 * Assistant) quando o IP sozinho nao diz "quem" e so "de onde". */
static char s_client_agent[64] = "";

/* -------------------------------------------------------------------------
 * Eventing UPnP (GENA) -- SUBSCRIBE/NOTIFY. Sem isso, o control point (ex.:
 * Music Assistant) nunca sabe que o estado mudou (Play/Pause/Stop) a menos
 * que fique perguntando (GetTransportInfo) por conta propria -- foi
 * reportado ao vivo que o MA ficava mostrando "tocando" depois de um Pause
 * de verdade, porque ele espera receber um NOTIFY, nao ficar sondando.
 * Implementacao minima: guarda so UMA assinatura (o unico control point
 * real deste projeto e o MA) e manda o NOTIFY numa task dedicada, nunca
 * direto do handler SOAP (que nao pode bloquear numa chamada de rede de
 * saida). */
static char s_event_callback_url[128] = "";
static char s_event_sid[48] = "";
static volatile uint32_t s_event_seq = 0;
static volatile bool s_event_subscribed = false;
static SemaphoreHandle_t s_event_notify_sem = NULL;
static TaskHandle_t s_event_task_handle = NULL;
/* Definida perto da task de eventing, bem mais abaixo -- prototipo aqui
 * porque e usada tanto por dlna_fetch_and_play() (fim natural de faixa)
 * quanto pelos handlers SOAP (Play/Pause/Stop), ambos antes da definicao
 * real no arquivo. */
static void dlna_notify_state_change_async(void);

/* Incrementado a cada Play/SetAVTransportURI -- a task de busca compara seu
 * proprio "my_generation" contra isso periodicamente; se mudou, uma
 * transicao mais nova ja superou essa busca, e ela aborta sozinha. Mesma
 * ideia do s_slim_session_id que o Slimproto usava, mas sem precisar criar
 * task nova a cada faixa (ver comentario em dlna_fetch_task). */
static volatile uint32_t s_target_generation = 0;
static TaskHandle_t s_fetch_task_handle = NULL;

/* Pause de verdade (nao um Stop disfarcado): a conexao HTTP e a decodificacao
 * EM ANDAMENTO ficam vivas, intactas, com todo o estado local (decoder FLAC,
 * buffers, posicao no arquivo remoto) -- so a task de busca para de ler mais
 * da rede (o socket fica com dado parado no buffer do SO/do lado do MA,
 * back-pressure de TCP natural, sem gastar CPU nem estourar memoria) e o
 * codec e mutado. Um Play seguinte na MESMA faixa so acorda a task de onde
 * ela parou -- sem reabrir conexao, sem perder a posicao. Isso e o que o
 * Music Assistant realmente espera: confirmado lendo o dlna/player.py real
 * do MA -- o metodo play() dele so manda a acao "Play" de novo pra MESMA
 * URI, sem nenhum seek/offset; ele conta com o RENDERER preservar onde
 * parou, exatamente como um player DLNA de hardware faz. */
static volatile bool s_fetch_paused = false;
static SemaphoreHandle_t s_fetch_resume_sem = NULL;

/* -------------------------------------------------------------------------
 * Buffer circular + task dedicada de I2S -- mesmo padrao ja comprovado em
 * bt_audio.c: desacopla a busca de rede (sujeita a jitter do Wi-Fi/HTTP) da
 * entrega ao codec (precisa de cadencia estavel pra nao engasgar). Tudo
 * criado UMA UNICA VEZ em dlna_renderer_init() -- nao ha task nova por
 * faixa nem por sessao, entao a classe inteira de corrida de reuso de stack
 * estatico que derrubou o Slimproto nao existe aqui: e sempre a mesma
 * instancia de task, do boot ate o dispositivo desligar. */
#define DLNA_RINGBUF_HIGHEST_WATER_LEVEL  (128 * 1024)
#define DLNA_RINGBUF_PREFETCH_WATER_LEVEL (80 * 1024)

typedef enum {
    DLNA_RINGBUF_MODE_PREFETCHING,
    DLNA_RINGBUF_MODE_PROCESSING,
    DLNA_RINGBUF_MODE_DROPPING,
} dlna_ringbuf_mode_t;

static uint8_t *s_ringbuf_storage = NULL;
static StaticRingbuffer_t s_ringbuf_struct;
static RingbufHandle_t s_ringbuf = NULL;
static SemaphoreHandle_t s_i2s_write_sem = NULL;
static TaskHandle_t s_i2s_task_handle = NULL;
static volatile dlna_ringbuf_mode_t s_ringbuf_mode = DLNA_RINGBUF_MODE_PREFETCHING;

/* Escreve PCM 16 bits (ja no formato que audio_codec_write espera) no ring
 * buffer -- chamada pela task de busca/decode. Mesmo padrao de backpressure
 * de bt_audio.c: se o consumidor (task de I2S) nao esta dando conta, dropa
 * em vez de bloquear a task de busca (que precisa continuar lendo da rede
 * pra nao acumular atraso). */
static size_t dlna_write_ringbuf(const uint8_t *data, size_t size)
{
    if (s_ringbuf == NULL || size == 0) {
        return 0;
    }

    /* CRITICO (achado ao vivo, 2026-08-16): diferente do bt_audio.c (de
     * onde este padrao foi copiado), o produtor aqui e a NOSSA task de
     * busca/decode, nao um callback da pilha Bluetooth que nao pode
     * bloquear. Rede local + FLAC decodificam MUITO mais rapido que o
     * tempo real de reproducao, entao um envio nao-bloqueante (timeout 0,
     * como o bt_audio usa) enche o buffer de 128KB em menos de 1s e fica
     * descartando dado sem parar dali pra frente -- o log mostrou isso ao
     * vivo ("ring buffer cheio, descartando" repetindo sem fim). Um envio
     * BLOQUEANTE aqui vira controle de fluxo de verdade: a task de busca
     * espera a task de I2S abrir espaco (consumindo no ritmo real do
     * audio) antes de continuar lendo/decodificando mais da rede -- exatamente
     * o comportamento certo pra essa arquitetura (buffer de tamanho fixo +
     * produtor mais rapido que o consumidor). Timeout de 2s (nao
     * portMAX_DELAY) so como rede de seguranca, pra dlna_should_abort() no
     * loop de fora conseguir ser reavaliado periodicamente em vez de ficar
     * bloqueado pra sempre se a task de I2S alguma vez travar. */
    BaseType_t done = xRingbufferSend(s_ringbuf, (void *)data, size, pdMS_TO_TICKS(2000));
    if (!done) {
        ESP_LOGW(TAG, "ring buffer de audio DLNA nao liberou espaco em 2s, descartando");
        return 0;
    }

    if (s_ringbuf_mode == DLNA_RINGBUF_MODE_PREFETCHING) {
        size_t used = 0;
        vRingbufferGetInfo(s_ringbuf, NULL, NULL, NULL, NULL, &used);
        if (used >= DLNA_RINGBUF_PREFETCH_WATER_LEVEL) {
            s_ringbuf_mode = DLNA_RINGBUF_MODE_PROCESSING;
            xSemaphoreGive(s_i2s_write_sem);
        }
    }
    return size;
}

static void dlna_i2s_task_handler(void *arg)
{
    const size_t max_chunk = 240 * 6;

    for (;;) {
        if (xSemaphoreTake(s_i2s_write_sem, portMAX_DELAY) == pdTRUE) {
            for (;;) {
                size_t item_size = 0;
                uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
                    s_ringbuf, &item_size, pdMS_TO_TICKS(20), max_chunk);
                if (item_size == 0) {
                    s_ringbuf_mode = DLNA_RINGBUF_MODE_PREFETCHING;
                    break;
                }
                audio_agc_feed((const int16_t *)data, item_size / sizeof(int16_t));
                size_t written = 0;
                audio_codec_write(data, item_size, &written);
                vRingbufferReturnItem(s_ringbuf, data);
            }
        }
    }
}

/* Descarta qualquer resto de audio de uma sessao anterior -- chamado no
 * inicio de cada nova busca (dlna_fetch_and_play), antes do primeiro byte
 * decodificado, pra nao misturar o fim de uma faixa com o comeco da
 * proxima. */
static void dlna_ringbuf_flush(void)
{
    if (s_ringbuf == NULL) {
        return;
    }
    size_t item_size;
    void *data;
    while ((data = xRingbufferReceive(s_ringbuf, &item_size, 0)) != NULL) {
        vRingbufferReturnItem(s_ringbuf, data);
    }
    s_ringbuf_mode = DLNA_RINGBUF_MODE_PREFETCHING;
}

/* -------------------------------------------------------------------------
 * SCPD (descricao de servico) -- minimas, so com as actions que este
 * marco realmente implementa.
 * ------------------------------------------------------------------------- */

static const char AVTRANSPORT_SCPD[] =
    "<?xml version=\"1.0\"?>"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
    "<specVersion><major>1</major><minor>0</minor></specVersion>"
    "<actionList>"
    "<action><name>SetAVTransportURI</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
    "<argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>Play</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>SetNextAVTransportURI</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>NextURI</name><direction>in</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>"
    "<argument><name>NextURIMetaData</name><direction>in</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>Pause</name></action>"
    "<action><name>Stop</name></action>"
    "<action><name>GetTransportInfo</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>"
    "<argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>"
    "<argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetPositionInfo</name></action>"
    "<action><name>GetMediaInfo</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>NrTracks</name><direction>out</direction><relatedStateVariable>NumberOfTracks</relatedStateVariable></argument>"
    "<argument><name>MediaDuration</name><direction>out</direction><relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument>"
    "<argument><name>CurrentURI</name><direction>out</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
    "<argument><name>CurrentURIMetaData</name><direction>out</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
    "<argument><name>NextURI</name><direction>out</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>"
    "<argument><name>NextURIMetaData</name><direction>out</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>"
    "</argumentList></action>"
    "</actionList>"
    "<serviceStateTable>"
    "<stateVariable sendEvents=\"no\"><name>TransportState</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>TransportStatus</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>TransportPlaySpeed</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>NextAVTransportURI</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>NextAVTransportURIMetaData</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>NumberOfTracks</name><dataType>ui4</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>CurrentMediaDuration</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>CurrentTrackURI</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>CurrentTrackMetaData</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>CurrentTrackDuration</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
    "<stateVariable sendEvents=\"yes\"><name>LastChange</name><dataType>string</dataType></stateVariable>"
    "</serviceStateTable></scpd>";

static const char RENDERINGCONTROL_SCPD[] =
    "<?xml version=\"1.0\"?>"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
    "<specVersion><major>1</major><minor>0</minor></specVersion>"
    "<actionList>"
    "<action><name>SetVolume</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
    "<argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetVolume</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
    "<argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>SetMute</name></action>"
    "</actionList>"
    "<serviceStateTable>"
    "<stateVariable sendEvents=\"no\"><name>Volume</name><dataType>ui2</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Channel</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
    "</serviceStateTable></scpd>";

static const char CONNECTIONMANAGER_SCPD[] =
    "<?xml version=\"1.0\"?>"
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
    "<specVersion><major>1</major><minor>0</minor></specVersion>"
    "<actionList>"
    "<action><name>GetProtocolInfo</name></action>"
    "<action><name>GetCurrentConnectionIDs</name></action>"
    "<action><name>GetCurrentConnectionInfo</name></action>"
    "</actionList>"
    "<serviceStateTable>"
    "<stateVariable sendEvents=\"no\"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>"
    "</serviceStateTable></scpd>";

/* -------------------------------------------------------------------------
 * Helpers HTTP/SOAP
 * ------------------------------------------------------------------------- */

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = req->content_len;
    if (total < 0 || (size_t)total >= buf_size) {
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';
    return ESP_OK;
}

/* Extrai o conteudo de <Tag>...</Tag> -- suficiente pros corpos SOAP
 * pequenos e previsiveis que control points DLNA mandam; evita precisar
 * de um parser XML completo pra esse escopo minimo. */
static bool extract_xml_tag(const char *xml, const char *tag, char *out, size_t out_size)
{
    char open_tag[48], close_tag[48];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    const char *start = strstr(xml, open_tag);
    if (!start) {
        return false;
    }
    start += strlen(open_tag);
    const char *end = strstr(start, close_tag);
    if (!end || end < start) {
        return false;
    }
    size_t len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

/* Acha um atributo do tipo attr="valor" em qualquer lugar do XML -- usado
 * so pra pegar duration="H:MM:SS..." de dentro de <res ...>, que
 * extract_xml_tag() nao serve pra isso (e um atributo, nao um par de
 * tags). Como "duration" so aparece nesse atributo dentro de um DIDL-Lite
 * normal, procurar sem se importar com qual tag esta dentro e seguro o
 * suficiente pro escopo minimo deste parser. */
static bool extract_xml_attr(const char *xml, const char *attr, char *out, size_t out_size)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "%s=\"", attr);
    const char *start = strstr(xml, needle);
    if (!start) {
        return false;
    }
    start += strlen(needle);
    const char *end = strchr(start, '"');
    if (!end || end < start) {
        return false;
    }
    size_t len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

/* SOAPAction header vem tipo: "urn:schemas-upnp-org:service:X:1#ActionName" */
static const char *soap_action_name(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (httpd_req_get_hdr_value_str(req, "SOAPAction", buf, buf_size) != ESP_OK) {
        return NULL;
    }
    char *hash = strchr(buf, '#');
    if (!hash) {
        return NULL;
    }
    hash++;
    char *quote = strchr(hash, '"');
    if (quote) {
        *quote = '\0';
    }
    return hash;
}

static void soap_respond(httpd_req_t *req, const char *service_type, const char *action, const char *body_extra)
{
    /* Estatico e grande: GetPositionInfo/GetMediaInfo agora carregam DIDL-Lite
     * escapado da faixa atual e passam MUITO dos 512 bytes de antes. Estatico
     * (nao na pilha) porque o httpd do DLNA atende uma requisicao por vez numa
     * unica task -- e 3KB na pilha dela, que ja carrega body[2048], seria
     * pedir outro stack overflow. */
    char *resp = s_soap_resp;
    const size_t resp_size = DLNA_BUF_SOAP_RESP;
    int len = snprintf(resp, resp_size,
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse></s:Body></s:Envelope>",
        action, service_type, body_extra ? body_extra : "", action);
    /* snprintf devolve o tamanho QUE CABERIA, nao o escrito -- mandar isso
     * direto pro httpd_resp_send faria ele ler fora do buffer se truncasse. */
    if (len < 0) {
        len = 0;
    } else if ((size_t)len >= resp_size) {
        len = (int)resp_size - 1;
        logger_log(ESP_LOG_WARN, TAG, "dlna: resposta SOAP de %s truncada", action);
    }
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    httpd_resp_send(req, resp, len);
}

static void soap_fault(httpd_req_t *req)
{
    static const char *resp =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
        "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
        "<errorCode>401</errorCode><errorDescription>Invalid Action</errorDescription>"
        "</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    httpd_resp_sendstr(req, resp);
}

/* -------------------------------------------------------------------------
 * Motor de reproducao: busca HTTP (esp_http_client) + decodifica (WAV/PCM
 * direto ou FLAC via flac_stream_decoder) + entrega no ring buffer.
 *
 * Task UNICA, persistente (criada uma vez em dlna_renderer_init, nunca
 * destruida) -- dorme bloqueada em ulTaskNotifyTake() quando ociosa, acorda
 * quando dlna_engine_play() sinaliza uma URI nova. Uma troca de faixa
 * rapida (Play chamado de novo enquanto uma busca anterior ainda esta em
 * andamento) so incrementa s_target_generation -- a busca em andamento
 * detecta isso (dlna_should_abort, checado a cada leitura de rede) e sai
 * sozinha; a proxima volta do loop principal ja pega a URI/geracao nova. Sem
 * criar/destruir task nenhuma, entao sem a corrida de reuso de stack
 * estatico que derrubou o Slimproto (ver docs/slimproto_retrospective.md).
 * ------------------------------------------------------------------------- */

#define DLNA_HTTP_TIMEOUT_MS      5000  /* nenhuma chamada de rede pode travar a task pra sempre */
#define DLNA_FETCH_TASK_STACK     8192  /* mesmo valor validado ao vivo pro decode FLAC (ver memoria do projeto) */
#define DLNA_FLAC_IN_CAP          4096

static bool dlna_should_abort(uint32_t my_generation)
{
    if (s_target_generation != my_generation) {
        return true; /* uma transicao mais nova ja superou esta busca */
    }
    /* BT sempre tem prioridade -- mesma regra aplicada em todo o projeto
     * (bt_audio.c/web_server.c/mqtt_ha.c). */
    bt_audio_status_t bt;
    bt_audio_get_status(&bt);
    return bt.connected;
}

/* Bloqueia a task de busca EXATAMENTE onde ela esta (conexao HTTP, decoder
 * FLAC e todo o estado local da funcao continuam intactos na pilha) durante
 * um Pause de verdade -- ver comentario em s_fetch_paused. Reavalia
 * periodicamente (nao so quando sinalizada) pra continuar pegando prioridade
 * de Bluetooth ou um Stop/nova-faixa mesmo enquanto pausada. Retorna false
 * se deve abortar (BT conectou, ou um Stop/nova URI invalidou a geracao
 * enquanto esperava); true se deve continuar de onde parou. */
static bool dlna_wait_while_paused(uint32_t my_generation)
{
    while (s_fetch_paused) {
        if (dlna_should_abort(my_generation)) {
            return false;
        }
        xSemaphoreTake(s_fetch_resume_sem, pdMS_TO_TICKS(500));
    }
    return !dlna_should_abort(my_generation);
}

/* Le exatamente `len` bytes (ou menos se a conexao fechar/der timeout) --
 * esp_http_client_read() pode devolver menos do que pedido mesmo com dado
 * ainda por vir, entao repete ate encher ou desistir. */
static int dlna_http_read_exact(esp_http_client_handle_t client, uint8_t *buf, size_t len, uint32_t my_generation)
{
    size_t got = 0;
    while (got < len) {
        if (dlna_should_abort(my_generation)) {
            return -1;
        }
        int n = esp_http_client_read(client, (char *)buf + got, len - got);
        if (n <= 0) {
            return (int)got; /* fim de stream ou erro -- devolve o que conseguiu */
        }
        got += (size_t)n;
    }
    return (int)got;
}

static bool dlna_http_skip(esp_http_client_handle_t client, size_t len, uint32_t my_generation)
{
    uint8_t scratch[64];
    while (len > 0) {
        size_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);
        int n = dlna_http_read_exact(client, scratch, chunk, my_generation);
        if (n <= 0) {
            return false;
        }
        len -= (size_t)n;
    }
    return true;
}

/* Parser minimo de cabecalho WAV/RIFF -- le so o que precisa (chunk "fmt "),
 * pula qualquer outro chunk sem guardar (LIST, fact, etc.) ate achar "data",
 * onde para: a partir dali o stream e PCM cru, pronto pra repassar direto.
 * Buffers pequenos e fixos de proposito -- sem alocacao, sem tamanho
 * dependente do conteudo (defesa contra um cabecalho malformado/hostil). */
/* magic4 sao os 4 primeiros bytes do corpo, ja lidos por dlna_fetch_and_play
 * pra decidir o formato (ver comentario la) -- "RIFF" confirmado, so falta
 * ler o resto do cabecalho fixo (8 bytes: tamanho + "WAVE"). */
static bool dlna_wav_parse_header(esp_http_client_handle_t client, uint32_t my_generation,
                                   const uint8_t magic4[4],
                                   uint32_t *out_rate, uint16_t *out_channels, uint16_t *out_bits)
{
    uint8_t riff[12];
    memcpy(riff, magic4, 4);
    if (dlna_http_read_exact(client, riff + 4, 8, my_generation) != 8) {
        return false;
    }
    if (memcmp(riff + 8, "WAVE", 4) != 0) {
        logger_log(ESP_LOG_WARN, TAG, "dlna: cabecalho RIFF sem WAVE");
        return false;
    }

    bool have_fmt = false;
    for (;;) {
        uint8_t chunk_hdr[8];
        if (dlna_http_read_exact(client, chunk_hdr, sizeof(chunk_hdr), my_generation) != (int)sizeof(chunk_hdr)) {
            return false;
        }
        uint32_t chunk_size = (uint32_t)chunk_hdr[4] | ((uint32_t)chunk_hdr[5] << 8) |
                               ((uint32_t)chunk_hdr[6] << 16) | ((uint32_t)chunk_hdr[7] << 24);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            uint8_t fmt[16] = {0};
            size_t to_read = chunk_size < sizeof(fmt) ? chunk_size : sizeof(fmt);
            if (dlna_http_read_exact(client, fmt, to_read, my_generation) != (int)to_read) {
                return false;
            }
            if (chunk_size > to_read && !dlna_http_skip(client, chunk_size - to_read, my_generation)) {
                return false;
            }
            *out_channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            *out_rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            *out_bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
            have_fmt = true;
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            /* Posicionado bem no comeco do PCM -- devolve pro chamador
             * comecar a ler as amostras a partir daqui. */
            return have_fmt;
        } else {
            if (!dlna_http_skip(client, chunk_size, my_generation)) {
                return false;
            }
        }
        if (chunk_size % 2 == 1) {
            /* Chunks RIFF sao alinhados a 16 bits -- 1 byte de padding se o
             * tamanho for impar. */
            dlna_http_skip(client, 1, my_generation);
        }
    }
}

/* Extrai rate/channels de "audio/L16;rate=44100;channels=2" (RFC 2586) --
 * L16 nao tem cabecalho proprio, so os parametros do Content-Type dizem o
 * formato real. Valores default (44100/2) se os parametros nao vierem, pra
 * nao travar com um servidor que declara L16 sem eles. */
static void dlna_parse_l16_params(const char *content_type, uint32_t *out_rate, uint16_t *out_channels)
{
    *out_rate = 44100;
    *out_channels = 2;
    const char *rate_p = strstr(content_type, "rate=");
    if (rate_p) {
        *out_rate = (uint32_t)atoi(rate_p + 5);
    }
    const char *ch_p = strstr(content_type, "channels=");
    if (ch_p) {
        *out_channels = (uint16_t)atoi(ch_p + 9);
    }
}

/* Converte um bloco de PCM 16 bits ja no formato certo (mono vira estereo
 * duplicando o canal) e manda pro ring buffer. */
static void dlna_emit_pcm16(const int16_t *samples, size_t sample_count, uint16_t channels)
{
    if (channels == 2) {
        dlna_write_ringbuf((const uint8_t *)samples, sample_count * sizeof(int16_t));
        return;
    }
    /* Mono (ou qualquer coisa != 2 canais, tratada como mono) -- duplica
     * cada amostra pros dois canais. Buffer pequeno na pilha, processado em
     * lotes pra nao precisar de heap aqui. */
    int16_t stereo[128];
    size_t i = 0;
    while (i < sample_count) {
        size_t batch = 0;
        while (i < sample_count && batch < sizeof(stereo) / sizeof(stereo[0]) - 1) {
            stereo[batch++] = samples[i];
            stereo[batch++] = samples[i];
            i++;
        }
        dlna_write_ringbuf((const uint8_t *)stereo, batch * sizeof(int16_t));
    }
}

/* Caminho WAV/PCM/L16: sem decoder, so repassa (com truncamento de 24->16
 * bits quando for o caso -- mesma convencao ja usada pelo decoder FLAC:
 * pega os 2 bytes mais significativos de cada amostra). */
static void dlna_stream_pcm(esp_http_client_handle_t client, uint32_t my_generation,
                             uint32_t sample_rate, uint16_t channels, uint16_t bits, bool big_endian)
{
    audio_codec_reconfigure_clock(sample_rate);
    logger_log(ESP_LOG_INFO, TAG, "dlna: PCM %" PRIu32 " Hz, %u canal(is), %u bits", sample_rate, channels, bits);
    audio_codec_set_mute(false);
    relay_control_notify_playing(true);

    uint8_t in_buf[1024];
    int16_t out_buf[512];

    for (;;) {
        if (dlna_should_abort(my_generation)) {
            return;
        }
        if (s_fetch_paused && !dlna_wait_while_paused(my_generation)) {
            return;
        }
        if (bits == 16) {
            int n = esp_http_client_read(client, (char *)in_buf, sizeof(in_buf));
            if (n <= 0) {
                return; /* fim de stream ou erro/timeout */
            }
            size_t sample_count = (size_t)n / sizeof(int16_t);
            if (big_endian) {
                for (size_t i = 0; i < sample_count; i++) {
                    uint8_t hi = in_buf[i * 2];
                    uint8_t lo = in_buf[i * 2 + 1];
                    out_buf[i] = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
                }
                dlna_emit_pcm16(out_buf, sample_count, channels);
            } else {
                dlna_emit_pcm16((const int16_t *)in_buf, sample_count, channels);
            }
        } else if (bits == 24) {
            /* 3 bytes por amostra -- le em multiplos de 3, converte pros 2
             * bytes mais significativos de cada uma (mesma convencao do
             * FLAC, ver flac_stream_decoder.h). */
            size_t want = (sizeof(in_buf) / 3) * 3;
            int n = esp_http_client_read(client, (char *)in_buf, want);
            if (n <= 0) {
                return;
            }
            size_t sample_count = (size_t)n / 3;
            for (size_t i = 0; i < sample_count; i++) {
                out_buf[i] = (int16_t)((uint16_t)in_buf[i * 3 + 1] | ((uint16_t)in_buf[i * 3 + 2] << 8));
            }
            dlna_emit_pcm16(out_buf, sample_count, channels);
        } else {
            logger_log(ESP_LOG_WARN, TAG, "dlna: profundidade de bits nao suportada (%u)", bits);
            return;
        }
    }
}

/* magic4: os 4 primeiros bytes do corpo ("fLaC"), ja lidos por
 * dlna_fetch_and_play pra identificar o formato -- semeia o buffer de
 * entrada do decoder com eles em vez de le-los de novo (ja foram
 * consumidos do socket). */
static void dlna_stream_flac(esp_http_client_handle_t client, uint32_t my_generation, const uint8_t magic4[4])
{
    flac_stream_decoder_t *dec = flac_stream_decoder_create();
    if (dec == NULL) {
        logger_log(ESP_LOG_ERROR, TAG, "dlna: falha ao criar decoder FLAC");
        return;
    }

    uint8_t *in_buf = heap_caps_malloc(DLNA_FLAC_IN_CAP, MALLOC_CAP_SPIRAM);
    int32_t *out_buf = NULL;
    int16_t *pcm_buf = NULL;
    size_t out_cap_samples = 0;
    size_t in_len = 0;
    uint16_t channels = 2;
    bool unmuted = false;

    if (in_buf == NULL) {
        logger_log(ESP_LOG_ERROR, TAG, "dlna: sem PSRAM pro buffer de entrada FLAC");
        goto cleanup;
    }
    memcpy(in_buf, magic4, 4);
    in_len = 4;

    for (;;) {
        if (dlna_should_abort(my_generation)) {
            break;
        }
        if (s_fetch_paused && !dlna_wait_while_paused(my_generation)) {
            break;
        }
        if (in_len < DLNA_FLAC_IN_CAP) {
            int n = esp_http_client_read(client, (char *)in_buf + in_len, DLNA_FLAC_IN_CAP - in_len);
            if (n < 0) {
                break; /* erro/timeout de rede */
            }
            in_len += (size_t)n;
            if (n == 0 && in_len == 0) {
                break; /* fim de stream, nada mais a decodificar */
            }
        }

        size_t consumed = 0, decoded = 0;
        flac_stream_result_t r = flac_stream_decoder_feed(dec, in_buf, in_len, out_buf, out_cap_samples,
                                                            &consumed, &decoded);
        if (consumed > 0 && consumed <= in_len) {
            memmove(in_buf, in_buf + consumed, in_len - consumed);
            in_len -= consumed;
        }

        if (r == FLAC_STREAM_ERROR) {
            logger_log(ESP_LOG_WARN, TAG, "dlna: erro no decode FLAC");
            break;
        }
        if (r == FLAC_STREAM_END_OF_STREAM) {
            break;
        }
        if (r == FLAC_STREAM_HEADER_READY) {
            uint32_t rate = flac_stream_decoder_sample_rate(dec);
            channels = (uint16_t)flac_stream_decoder_channels(dec);
            out_cap_samples = flac_stream_decoder_output_buffer_samples(dec);
            out_buf = heap_caps_malloc(out_cap_samples * sizeof(int32_t), MALLOC_CAP_SPIRAM);
            pcm_buf = heap_caps_malloc(out_cap_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (out_buf == NULL || pcm_buf == NULL) {
                logger_log(ESP_LOG_ERROR, TAG, "dlna: sem PSRAM pro buffer de saida FLAC");
                break;
            }
            audio_codec_reconfigure_clock(rate);
            logger_log(ESP_LOG_INFO, TAG, "dlna: FLAC %" PRIu32 " Hz, %u bits, %u canal(is)",
                       rate, (unsigned)flac_stream_decoder_bits_per_sample(dec), channels);
            continue;
        }
        if (r == FLAC_STREAM_OK && decoded > 0) {
            if (!unmuted) {
                audio_codec_set_mute(false);
                relay_control_notify_playing(true);
                unmuted = true;
            }
            for (size_t i = 0; i < decoded; i++) {
                pcm_buf[i] = (int16_t)(out_buf[i] >> 16);
            }
            dlna_emit_pcm16(pcm_buf, decoded, channels);
        }
        /* FLAC_STREAM_NEED_MORE_DATA -- so volta pro topo do loop e le mais. */
    }

cleanup:
    if (in_buf) {
        heap_caps_free(in_buf);
    }
    if (out_buf) {
        heap_caps_free(out_buf);
    }
    if (pcm_buf) {
        heap_caps_free(pcm_buf);
    }
    flac_stream_decoder_destroy(dec);
}

/* continuation=true quando esta emendando na proxima faixa da fila (ver
 * dlna_advance_to_next_track): nesse caso NAO descarta o que ainda esta no
 * ring buffer -- e justamente a cauda da faixa anterior, que precisa
 * terminar de tocar pra transicao ficar continua (gapless). */
static void dlna_fetch_and_play(const char *uri, uint32_t my_generation, bool continuation)
{
    if (!continuation) {
        dlna_ringbuf_flush();
    }

    esp_http_client_config_t config = {
        .url = uri,
        .method = HTTP_METHOD_GET,
        .timeout_ms = DLNA_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        logger_log(ESP_LOG_ERROR, TAG, "dlna: falha ao criar cliente HTTP");
        return;
    }
    /* REVERTIDO (achado ao vivo, 2026-08-16): mandar "Icy-MetaData: 1" nao
     * trouxe o icy-metaint na resposta (MA nao honra esse pedido nesse
     * endpoint), e passou a coincidir com erros reais de decode FLAC logo
     * depois -- suspeita forte de que o MA passou a injetar bytes de
     * metadado no meio do stream sem avisar via cabeçalho, corrompendo o
     * que a gente tratava como audio puro. Sem beneficio nenhum (a
     * informacao que queriamos nunca veio) e com risco real de corromper
     * playback -- volta pro pedido original sem esse cabecalho. */

    bool played_anything = false;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        logger_log(ESP_LOG_WARN, TAG, "dlna: falha ao conectar em %s", uri);
        goto done;
    }

    {
        int64_t content_length = esp_http_client_fetch_headers(client);
        (void)content_length; /* nao usado -- lemos ate o servidor fechar/EOF, chunked ou nao */
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            logger_log(ESP_LOG_WARN, TAG, "dlna: servidor respondeu %d pra %s", status, uri);
            goto done;
        }

        char *content_type = NULL;
        esp_http_client_get_header(client, "Content-Type", &content_type);
        char ct_lower[80] = "";
        if (content_type != NULL) {
            strlcpy(ct_lower, content_type, sizeof(ct_lower));
            for (char *p = ct_lower; *p; p++) {
                *p = (char)tolower((unsigned char)*p);
            }
        }
        /* Content-Type primeiro na mensagem, nao a URI -- LOGGER_MSG_MAX_LEN
         * (128 bytes, ver config.h) trunca mensagens longas, e as URIs de
         * stream do MA (ex.: /flow/...) sozinhas ja passam disso, cortando
         * o Content-Type (a parte realmente util pra diagnostico) fora. */
        logger_log(ESP_LOG_INFO, TAG, "dlna: Content-Type='%s' uri=%s", ct_lower, uri);


        if (dlna_should_abort(my_generation)) {
            goto done;
        }

        /* CRITICO (achado ao vivo, 2026-08-16): o proprio servidor de
         * streaming do Music Assistant (endpoint /flow/..., usado pra fila
         * continua/gapless) as vezes NAO manda Content-Type nenhum -- so
         * confiar nesse cabecalho, como a versao anterior deste codigo
         * fazia, caia sempre no "trata como WAV" por omissao e falhava com
         * qualquer outra coisa. Detecta pelos bytes reais do corpo (mesma
         * filosofia ja estabelecida neste projeto: confiar no conteudo de
         * verdade, nao em cabecalhos que um servidor pode nao mandar
         * direito -- ver historico do Slimproto sobre o MA ignorando o
         * parametro de formato da URL). Content-Type continua usado como
         * dica pro caso L16 (PCM cru, sem assinatura nenhuma nos bytes). */
        if (strstr(ct_lower, "l16") != NULL) {
            uint32_t rate;
            uint16_t channels;
            dlna_parse_l16_params(ct_lower, &rate, &channels);
            dlna_stream_pcm(client, my_generation, rate, channels, 16, true /* L16 e big-endian, RFC 2586 */);
            played_anything = true;
        } else {
            uint8_t magic[4] = {0};
            int got = dlna_http_read_exact(client, magic, sizeof(magic), my_generation);
            if (got != (int)sizeof(magic)) {
                logger_log(ESP_LOG_WARN, TAG, "dlna: stream fechou antes de mandar dado suficiente pra identificar o formato");
            } else if (memcmp(magic, "fLaC", 4) == 0) {
                dlna_stream_flac(client, my_generation, magic);
                played_anything = true;
            } else if (memcmp(magic, "RIFF", 4) == 0) {
                uint32_t rate = 0;
                uint16_t channels = 0, bits = 0;
                if (dlna_wav_parse_header(client, my_generation, magic, &rate, &channels, &bits)) {
                    dlna_stream_pcm(client, my_generation, rate, channels, bits, false);
                    played_anything = true;
                }
            } else if (memcmp(magic, "ID3", 3) == 0 || (magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0)) {
                /* ID3v2 (tag no inicio) ou sync word MPEG cru -- MP3. Sem
                 * decoder pra isso ainda neste firmware (so FLAC e
                 * WAV/PCM/L16) -- log claro em vez de tentar tocar lixo. */
                logger_log(ESP_LOG_WARN, TAG, "dlna: stream parece ser MP3 -- sem decoder MP3 neste firmware ainda");
            } else {
                logger_log(ESP_LOG_WARN, TAG,
                           "dlna: formato desconhecido (primeiros bytes: %02x %02x %02x %02x)",
                           magic[0], magic[1], magic[2], magic[3]);
            }
        }
    }

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* So mexe no estado "tocando" se ninguem mais novo assumiu -- mesma
     * regra ja usada no Slimproto pra nao cortar o comeco de uma faixa
     * nova numa corrida entre "sessao antiga terminando" e "sessao nova
     * comecando". */
    if (s_target_generation == my_generation) {
        /* Bluetooth assumiu no meio da faixa? Ai a fila NAO continua, mesmo
         * havendo proxima enfileirada. BUG REAL reportado: sem tratar esse
         * caso separado, o estado ficava PLAYING (a task ate promovia a
         * proxima faixa antes de abortar de novo) e nenhum NOTIFY saia -- o
         * control point seguia mostrando "tocando" enquanto o audio na
         * verdade era do celular. */
        bt_audio_status_t bt;
        bt_audio_get_status(&bt);
        const bool bt_took_over = bt.connected;

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        bool has_next = (s_next_uri[0] != '\0');
        bool stopping = bt_took_over || !has_next;
        if (stopping) {
            s_playing = false;
            s_transport_state = DLNA_STATE_STOPPED;
            s_playback_elapsed_base_us = 0;
            if (bt_took_over) {
                /* Descarta a fila: quando o BT sair, quem decide o que tocar
                 * de novo e o control point, nao um resto de fila antigo. */
                s_next_uri[0] = '\0';
            }
        }
        xSemaphoreGive(s_state_mutex);

        if (stopping) {
            /* Mute/rele SO quando paramos por conta propria. Cedendo pro
             * Bluetooth, quem manda no codec e no rele passa a ser o
             * bt_audio.c (ele mesmo faz set_mute/notify_playing conforme o
             * estado do celular) -- desligar o rele aqui cortaria o
             * amplificador justo quando o BT vai comecar a tocar. */
            if (!bt_took_over) {
                audio_codec_set_mute(true);
                relay_control_notify_playing(false);
            } else {
                logger_log(ESP_LOG_INFO, TAG,
                           "dlna: Bluetooth assumiu -- DLNA para e avisa o control point (STOPPED)");
            }
            dlna_notify_state_change_async();
        }
    }
    (void)played_anything;
}

/* Promove a faixa enfileirada por SetNextAVTransportURI a faixa atual --
 * chamada quando o stream da faixa corrente termina naturalmente. Troca URI
 * e TODOS os metadados de uma vez, sob o mesmo mutex, pra /api/status e
 * GetPositionInfo nunca observarem um estado meio-trocado (titulo novo com
 * duracao velha, por exemplo). Zera o cronometro: a posicao volta a contar
 * do zero pra faixa nova, que e exatamente o que faltava. Retorna false se
 * nao havia proxima faixa (fim da fila). */
static bool dlna_advance_to_next_track(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool have_next = (s_next_uri[0] != '\0');
    if (have_next) {
        strlcpy(s_current_uri, s_next_uri, sizeof(s_current_uri));
        strlcpy(s_track, s_next_track, sizeof(s_track));
        strlcpy(s_artist, s_next_artist, sizeof(s_artist));
        strlcpy(s_album, s_next_album, sizeof(s_album));
        strlcpy(s_track_duration, s_next_duration, sizeof(s_track_duration));
        s_next_uri[0] = '\0';
        s_next_track[0] = '\0';
        s_next_artist[0] = '\0';
        s_next_album[0] = '\0';
        strlcpy(s_next_duration, "00:00:00", sizeof(s_next_duration));
        s_playback_elapsed_base_us = 0;
        s_playback_start_us = esp_timer_get_time();
        s_playing = true;
        s_transport_state = DLNA_STATE_PLAYING;
    }
    xSemaphoreGive(s_state_mutex);

    if (have_next) {
        logger_log(ESP_LOG_INFO, TAG, "dlna: proxima faixa da fila: %s", s_track);
        /* Avisa o control point que trocou -- ele espera esse evento pra
         * atualizar a interface dele sem ter que ficar sondando. */
        dlna_notify_state_change_async();
    }
    return have_next;
}

static void dlna_fetch_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Loop interno: quando o stream da faixa atual termina, ja emenda
         * na proxima que o control point enfileirou (SetNextAVTransportURI)
         * sem voltar a dormir -- e isso que da continuidade de fila com
         * metadados/posicao corretos por faixa. */
        bool continuation = false;
        for (;;) {
            uint32_t my_generation;
            char uri[256];
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            my_generation = s_target_generation;
            strlcpy(uri, s_current_uri, sizeof(uri));
            xSemaphoreGive(s_state_mutex);

            if (uri[0] == '\0' || dlna_should_abort(my_generation)) {
                break;
            }
            dlna_fetch_and_play(uri, my_generation, continuation);

            if (s_target_generation != my_generation) {
                break; /* superado por Play/Stop/faixa nova -- nao emenda */
            }
            {
                /* BT com prioridade: nao emenda na proxima (o dlna_fetch_and_play
                 * acima ja marcou STOPPED e avisou o control point). Sem essa
                 * checagem a task promovia a faixa seguinte, marcava PLAYING e
                 * so entao abortava -- deixando o estado mentindo. */
                bt_audio_status_t bt;
                bt_audio_get_status(&bt);
                if (bt.connected) {
                    break;
                }
            }
            if (!dlna_advance_to_next_track()) {
                break; /* fim da fila */
            }
            continuation = true;
        }
    }
}

/* -------------------------------------------------------------------------
 * API do motor, chamada pelos handlers SOAP (AVTransport).
 * ------------------------------------------------------------------------- */

static void dlna_engine_play(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool have_uri = (s_current_uri[0] != '\0');
    /* Retomar de verdade: task de busca ainda esta viva, bloqueada
     * exatamente onde a pausa a pegou (mesma conexao HTTP, mesmo decoder
     * FLAC, mesma posicao no arquivo remoto) -- so acordar ela, sem
     * reiniciar nada. So conta como "retomar" se for a MESMA faixa que
     * ficou pausada (SetAVTransportURI com uma URI diferente ja teria
     * zerado s_transport_state pra STOPPED -- ver aquele handler). */
    bool resuming = have_uri && (s_transport_state == DLNA_STATE_PAUSED);
    if (have_uri) {
        s_playing = true;
        s_transport_state = DLNA_STATE_PLAYING;
        s_playback_start_us = esp_timer_get_time();
        if (!resuming) {
            /* Faixa nova (ou depois de Stop) -- essa sim precisa de uma
             * busca do zero. */
            s_target_generation++;
            s_playback_elapsed_base_us = 0;
        }
    }
    xSemaphoreGive(s_state_mutex);

    if (have_uri) {
        if (resuming) {
            s_fetch_paused = false;
            audio_codec_set_mute(false);
            if (s_fetch_resume_sem != NULL) {
                xSemaphoreGive(s_fetch_resume_sem);
            }
        } else if (s_fetch_task_handle != NULL) {
            xTaskNotifyGive(s_fetch_task_handle);
        }
    }
    dlna_notify_state_change_async();
}

static void dlna_engine_pause(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* So acumula o decorrido se estava mesmo tocando -- um segundo Pause
     * chegando sem Play no meio (ja visto ao vivo, provavelmente o MA
     * reenviando) NAO pode somar o mesmo intervalo de novo. */
    if (s_transport_state != DLNA_STATE_PAUSED) {
        s_playback_elapsed_base_us += esp_timer_get_time() - s_playback_start_us;
    }
    s_playing = false;
    s_transport_state = DLNA_STATE_PAUSED;
    /* NAO incrementa s_target_generation aqui -- e exatamente isso que
     * mantem a conexao/decodificacao vivas em vez de abortar (ver
     * s_fetch_paused). */
    s_fetch_paused = true;
    xSemaphoreGive(s_state_mutex);
    /* Muta na hora -- nao espera o ring buffer esvaziar sozinho (o que
     * aconteceria de qualquer forma em menos de 1s, mas silenciar direto no
     * codec e mais limpo e imediato). */
    audio_codec_set_mute(true);
    dlna_notify_state_change_async();
}

static void dlna_engine_stop(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_target_generation++;
    s_playing = false;
    s_transport_state = DLNA_STATE_STOPPED;
    s_current_uri[0] = '\0';
    s_next_uri[0] = '\0'; /* Stop limpa a fila inteira, nao so a faixa atual */
    s_playback_elapsed_base_us = 0;
    strlcpy(s_track_duration, "00:00:00", sizeof(s_track_duration));
    bool was_paused = s_fetch_paused;
    s_fetch_paused = false;
    xSemaphoreGive(s_state_mutex);
    if (was_paused && s_fetch_resume_sem != NULL) {
        /* Task de busca estava bloqueada esperando um resume -- acorda ela
         * pra reavaliar dlna_should_abort() (generation mudou acima) e
         * sair/fechar a conexao em vez de ficar presa pra sempre. */
        xSemaphoreGive(s_fetch_resume_sem);
    }
    dlna_notify_state_change_async();
}

/* -------------------------------------------------------------------------
 * Handlers HTTP
 * ------------------------------------------------------------------------- */

/* Resposta em pedacos (chunked) em vez de montar tudo num snprintf so:
 * evita um buffer grande na pilha e o -Werror=format-truncation do GCC
 * (que estima o pior caso pelo tamanho DECLARADO de s_friendly_name/
 * s_uuid, nao pelo conteudo real). */
static esp_err_t description_xml_handler(httpd_req_t *req)
{
    static const char PART1[] =
        "<?xml version=\"1.0\"?>"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion>"
        "<device>"
        "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
        "<friendlyName>";
    /* Descricao de dispositivo mais completa -- o Music Assistant mostrava
     * "unknown / unknown" no card do player. Ele inicializa o DeviceInfo com
     * esses placeholders e so troca quando consegue ler os campos do
     * description.xml; controlpoints/parsers UPnP costumam esperar o
     * conjunto completo (incluindo modelNumber/serialNumber/URLs e o
     * X_DLNADOC, que identifica o perfil DLNA do dispositivo). Sao campos
     * estaticos e baratos -- nao custam nada em RAM (const em flash). */
    static const char PART2[] =
        "</friendlyName>"
        "<manufacturer>DIY</manufacturer>"
        "<manufacturerURL>https://github.com/celioblak</manufacturerURL>"
        "<modelDescription>Receiver Bluetooth DIY (ESP32 + ES8388)</modelDescription>"
        "<modelName>ESP32 Audio Kit V2.2</modelName>"
        "<modelNumber>1.0.0</modelNumber>"
        "<modelURL>https://github.com/celioblak</modelURL>"
        "<serialNumber>";
    static const char PART2B[] =
        "</serialNumber>"
        "<dlna:X_DLNADOC xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">DMR-1.50</dlna:X_DLNADOC>"
        "<UDN>";
    static const char PART3[] =
        "</UDN>"
        "<serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
        "<SCPDURL>/AVTransport.xml</SCPDURL><controlURL>/AVTransport/control</controlURL>"
        "<eventSubURL>/AVTransport/event</eventSubURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
        "<SCPDURL>/RenderingControl.xml</SCPDURL><controlURL>/RenderingControl/control</controlURL>"
        "<eventSubURL>/RenderingControl/event</eventSubURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
        "<SCPDURL>/ConnectionManager.xml</SCPDURL><controlURL>/ConnectionManager/control</controlURL>"
        "<eventSubURL>/ConnectionManager/event</eventSubURL></service>"
        "</serviceList></device></root>";

    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    httpd_resp_send_chunk(req, PART1, sizeof(PART1) - 1);
    httpd_resp_send_chunk(req, s_friendly_name, strlen(s_friendly_name));
    httpd_resp_send_chunk(req, PART2, sizeof(PART2) - 1);
    /* Serial = sufixo do UUID (derivado do MAC) -- unico por dispositivo,
     * sem precisar de mais nenhum estado guardado. */
    httpd_resp_send_chunk(req, s_uuid + 5, strlen(s_uuid) - 5);
    httpd_resp_send_chunk(req, PART2B, sizeof(PART2B) - 1);
    httpd_resp_send_chunk(req, s_uuid, strlen(s_uuid));
    httpd_resp_send_chunk(req, PART3, sizeof(PART3) - 1);
    httpd_resp_send_chunk(req, NULL, 0); /* termina o chunked encoding */
    return ESP_OK;
}

static esp_err_t scpd_handler(httpd_req_t *req)
{
    const char *xml = (const char *)req->user_ctx;
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    httpd_resp_sendstr(req, xml);
    return ESP_OK;
}

/* Desescapa entidades XML basicas (&lt; &gt; &amp; &quot; &apos;) -- o
 * DIDL-Lite dentro de CurrentURIMetaData vem com essas entidades escapadas
 * (e um XML dentro de outro XML), precisa desescapar antes de conseguir
 * achar as tags internas (dc:title, upnp:artist, upnp:album) com
 * extract_xml_tag(). Em-lugar -- a string desescapada nunca fica maior que
 * a original. */
/* Codifica um code point Unicode em UTF-8. Devolve quantos bytes escreveu.
 * Usado pra transformar referencia numerica de XML (&#233;) no caractere de
 * verdade (é) -- ver xml_unescape(). Nunca cresce em relacao a entidade que
 * originou (a menor entidade possivel, "&#0;", ja tem 4 bytes; o maior
 * code point cabe em 4), entao e seguro fazer isso em-lugar. */
static size_t utf8_put(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Remove uma sequencia UTF-8 partida no fim da string. Depois de decodificar
 * acentos, os campos passam a ter caracteres multi-byte -- e um corte por
 * tamanho (extract_xml_tag ou strlcpy em s_track[64]) pode parar no meio de
 * um deles. Byte invalido ali quebraria o JSON do /api/status e o payload
 * MQTT, entao e melhor perder o ultimo caractere. */
static void utf8_trim_incomplete(char *s)
{
    size_t len = strlen(s);
    if (len == 0) {
        return;
    }
    size_t i = len;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
        i--; /* anda pra tras sobre bytes de continuacao (10xxxxxx) */
    }
    if (i == 0) {
        return;
    }
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need;
    if (lead < 0x80) {
        need = 1;
    } else if ((lead & 0xE0) == 0xC0) {
        need = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        need = 4;
    } else {
        s[i - 1] = '\0'; /* byte lider invalido */
        return;
    }
    if ((len - (i - 1)) < need) {
        s[i - 1] = '\0'; /* faltam bytes -- corta a sequencia incompleta */
    }
}

static void xml_unescape(char *s)
{
    char *out = s;
    while (*s) {
        if (strncmp(s, "&lt;", 4) == 0) {
            *out++ = '<';
            s += 4;
        } else if (strncmp(s, "&gt;", 4) == 0) {
            *out++ = '>';
            s += 4;
        } else if (strncmp(s, "&amp;", 5) == 0) {
            *out++ = '&';
            s += 5;
        } else if (strncmp(s, "&quot;", 6) == 0) {
            *out++ = '"';
            s += 6;
        } else if (strncmp(s, "&apos;", 6) == 0) {
            *out++ = '\'';
            s += 6;
        } else if (s[0] == '&' && s[1] == '#') {
            /* Referencia numerica de caractere: "&#233;" (decimal) ou
             * "&#xE9;" (hex). O Music Assistant manda acentos assim, NAO em
             * UTF-8 cru -- sem decodificar, "Quero Ver é Me Esquecer" chegava
             * na pagina/MQTT como "Quero Ver &#233; Me Esquecer". */
            char *p = s + 2;
            int base = 10;
            if (*p == 'x' || *p == 'X') {
                base = 16;
                p++;
            }
            const char *digits = p;
            while (*p != '\0' && *p != ';') {
                p++;
            }
            unsigned long cp = 0;
            if (*p == ';' && p > digits) {
                cp = strtoul(digits, NULL, base);
            }
            /* cp == 0 tambem cai aqui: um NUL cortaria a string no meio, e
             * caractere nulo nem e valido em XML -- trata como texto comum. */
            if (cp > 0 && cp <= 0x10FFFF) {
                out += utf8_put(out, (uint32_t)cp);
                s = p + 1;
            } else {
                *out++ = *s++;
            }
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

/* Inverso de xml_unescape() -- escapa &<>" pra montar o LastChange do
 * NOTIFY (um XML dentro de outro XML, igual o DIDL-Lite, so que no sentido
 * contrario: aqui SOMOS nos que geramos o XML interno e precisamos
 * escapa-lo antes de embutir no propertyset externo). out/in podem ser
 * buffers separados (nao em-lugar, ao contrario de xml_unescape) porque
 * escapar sempre CRESCE a string. */
static void xml_escape_into(char *out, size_t out_size, const char *in)
{
    size_t o = 0;
    for (; *in && o + 6 < out_size; in++) {
        switch (*in) {
            case '<': o += snprintf(out + o, out_size - o, "&lt;"); break;
            case '>': o += snprintf(out + o, out_size - o, "&gt;"); break;
            case '&': o += snprintf(out + o, out_size - o, "&amp;"); break;
            case '"': o += snprintf(out + o, out_size - o, "&quot;"); break;
            default: out[o++] = *in; break;
        }
    }
    out[o] = '\0';
}

/* Formata microsegundos como "H:MM:SS" (formato aceito pelo UPnP AVTransport
 * pros campos RelTime/AbsTime/TrackDuration -- nao precisa de zero-padding
 * nas horas). Usado pra reportar posicao de reproducao REAL em
 * GetPositionInfo, nao mais um "00:00:00" fixo. */
static void dlna_format_hms(int64_t us, char *out, size_t out_size)
{
    if (us < 0) {
        us = 0;
    }
    int64_t total_s = us / 1000000;
    int h = (int)(total_s / 3600);
    int m = (int)((total_s % 3600) / 60);
    int s = (int)(total_s % 60);
    snprintf(out, out_size, "%d:%02d:%02d", h, m, s);
}

/* Inverso de dlna_format_hms() -- so pra comparar duracao vs. posicao (ver
 * dlna_effective_duration() logo abaixo). */
static int64_t dlna_parse_hms_us(const char *hms)
{
    int h = 0, m = 0, s = 0;
    sscanf(hms, "%d:%d:%d", &h, &m, &s);
    return ((int64_t)h * 3600 + m * 60 + s) * 1000000;
}

/* A duracao/titulo/artista/album vem do DIDL-Lite da PRIMEIRA faixa
 * carregada via SetAVTransportURI -- mas o /flow do Music Assistant e um
 * stream de AUDIO CONTINUO (varias faixas encadeadas na MESMA conexao HTTP
 * E no mesmo encode FLAC, sem cabecalho novo em lugar nenhum -- confirmado
 * ao vivo de duas formas independentes: 1) mais de 11 minutos tocando sem
 * nenhum SetAVTransportURI novo do MA; 2) um scan byte-a-byte procurando um
 * segundo cabeçalho "fLaC" no meio do stream, que nunca apareceu mesmo
 * passando bem da duracao da primeira faixa). O MA simplesmente nao avisa o
 * renderer quando a faixa muda dentro do flow -- nao existe nenhum sinal,
 * nem SOAP nem no proprio stream, pra saber a duracao/titulo REAL da faixa
 * atual depois desse ponto. Reportar "desconhecido" (duracao "0:00:00",
 * convencao UPnP; titulo/artista/album vazios) e a unica opcao honesta,
 * depois de esgotadas as duas formas de deteccao. */
static bool dlna_metadata_still_valid(int64_t elapsed_us)
{
    int64_t duration_us = dlna_parse_hms_us(s_track_duration);
    return !(duration_us > 0 && elapsed_us >= duration_us);
}

static void dlna_effective_duration(int64_t elapsed_us, char *out, size_t out_size)
{
    if (!dlna_metadata_still_valid(elapsed_us)) {
        strlcpy(out, "0:00:00", out_size);
    } else {
        strlcpy(out, s_track_duration, out_size);
    }
}

/* Monta um DIDL-Lite minimo descrevendo a faixa ATUAL e devolve ja
 * XML-escapado, pronto pra entrar como conteudo de texto de
 * <TrackMetaData>/<CurrentURIMetaData> numa resposta SOAP ou de
 * CurrentTrackMetaData num evento LastChange.
 *
 * POR QUE ISSO EXISTE: quando a fila avanca sozinha
 * (dlna_advance_to_next_track), o control point precisa descobrir QUAL faixa
 * passou a tocar. Antes disso o firmware trocava de faixa corretamente mas
 * nunca dizia o nome dela pra ninguem -- o Music Assistant seguia mostrando
 * a primeira musica da fila na interface (sintoma reportado pelo usuario).
 * Os campos vao escapados individualmente antes de montar o DIDL, senao um
 * titulo com "&" ou "<" quebraria o XML depois que o cliente desescapasse. */
static const char *dlna_current_didl_escaped(void)
{
    char *out = s_didl_esc;
    const size_t out_size = DLNA_BUF_DIDL_ESC;
    char track[64], artist[64], album[64], duration[16], uri[256];
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strlcpy(track, s_track, sizeof(track));
    strlcpy(artist, s_artist, sizeof(artist));
    strlcpy(album, s_album, sizeof(album));
    strlcpy(duration, s_track_duration, sizeof(duration));
    strlcpy(uri, s_current_uri, sizeof(uri));
    xSemaphoreGive(s_state_mutex);

    if (uri[0] == '\0') {
        out[0] = '\0';
        return out;
    }

    static char t_e[144], a_e[144], al_e[144], u_e[300];
    xml_escape_into(t_e, sizeof(t_e), track);
    xml_escape_into(a_e, sizeof(a_e), artist);
    xml_escape_into(al_e, sizeof(al_e), album);
    xml_escape_into(u_e, sizeof(u_e), uri);

    char *didl = s_didl_raw;
    snprintf(didl, DLNA_BUF_DIDL_RAW,
             "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
             "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
             "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
             "<item id=\"0\" parentID=\"0\" restricted=\"1\">"
             "<dc:title>%s</dc:title>"
             "<upnp:artist>%s</upnp:artist>"
             "<dc:creator>%s</dc:creator>"
             "<upnp:album>%s</upnp:album>"
             "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
             "<res protocolInfo=\"http-get:*:audio/flac:*\" duration=\"%s\">%s</res>"
             "</item></DIDL-Lite>",
             t_e, a_e, a_e, al_e, duration, u_e);
    xml_escape_into(out, out_size, didl);
    return out;
}

/* Pega o IP remoto direto do socket TCP da requisicao (getpeername) -- nao
 * depende de nenhum cabecalho HTTP, funciona pra qualquer control point.
 * Chamada em toda acao de AVTransport (nao so SetAVTransportURI) pra manter
 * "quem esta conectado" atualizado mesmo em Play/Pause/Stop isolados. */
static void dlna_capture_client_ip(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return;
    }
    /* sockaddr_storage, nao sockaddr_in: este projeto tem IPv6 do lwIP
     * habilitado (CONFIG_LWIP_IPV6=1, default do ESP-IDF), entao o socket
     * aceito pelo httpd e dual-stack -- um cliente IPv4 real (ex.: Music
     * Assistant) chega como endereco IPv4-mapeado dentro de um
     * sockaddr_in6 (::ffff:a.b.c.d), nao um sockaddr_in puro. Ler os bytes
     * como se fossem sockaddr_in de proposito (achado ao vivo: sempre dava
     * "0.0.0.0", porque sin_addr de sockaddr_in cai por cima do campo
     * flowinfo do sockaddr_in6, que e zero) exigia esse tratamento. */
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &len) != 0) {
        return;
    }

    uint8_t b4[4] = {0};
    bool got_ip = false;
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&addr;
        uint32_t a = ntohl(a4->sin_addr.s_addr);
        b4[0] = (uint8_t)(a >> 24);
        b4[1] = (uint8_t)(a >> 16);
        b4[2] = (uint8_t)(a >> 8);
        b4[3] = (uint8_t)a;
        got_ip = true;
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&addr;
        const uint8_t *raw = a6->sin6_addr.s6_addr;
        bool v4_mapped = true;
        for (int i = 0; i < 10; i++) {
            if (raw[i] != 0) {
                v4_mapped = false;
                break;
            }
        }
        if (v4_mapped && raw[10] == 0xFF && raw[11] == 0xFF) {
            memcpy(b4, raw + 12, 4);
            got_ip = true;
        }
    }

    char agent[64] = "";
    httpd_req_get_hdr_value_str(req, "User-Agent", agent, sizeof(agent));
    char ip[16] = "";
    if (got_ip) {
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b4[0], b4[1], b4[2], b4[3]);
    } else {
        strlcpy(ip, "(ipv6)", sizeof(ip));
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool changed = strcmp(s_client_ip, ip) != 0 || strcmp(s_client_agent, agent) != 0;
    strlcpy(s_client_ip, ip, sizeof(s_client_ip));
    strlcpy(s_client_agent, agent, sizeof(s_client_agent));
    xSemaphoreGive(s_state_mutex);

    /* So loga quando muda -- essa funcao roda em toda acao SOAP (Play,
     * Pause, Stop...), nao so na primeira conexao. */
    if (changed) {
        logger_log(ESP_LOG_INFO, TAG, "dlna: control point %s (User-Agent: %s)",
                   ip, agent[0] ? agent : "(nenhum)");
    }
}

/* Acorda a task de eventing pra mandar um NOTIFY -- so sinaliza (nunca
 * bloqueia, seguro de chamar de dentro de qualquer handler SOAP). Semaforo
 * binario: varias mudancas de estado em sequencia rapida colapsam num unico
 * NOTIFY (a task sempre manda o estado mais recente quando acorda, nao uma
 * fila de estados antigos) -- suficiente pro proposito (control point so
 * precisa saber o estado ATUAL, nao o historico). */
static void dlna_notify_state_change_async(void)
{
    if (s_event_notify_sem != NULL) {
        xSemaphoreGive(s_event_notify_sem);
    }
}

/* O LastChange precisa carregar mais que o TransportState: quando a fila
 * avanca internamente (dlna_advance_to_next_track), o estado CONTINUA
 * "PLAYING" -- se o evento so disser isso, o control point nao tem como
 * perceber que a faixa mudou, nao atualiza a interface e, pior, nao
 * enfileira a proxima (visto ao vivo: depois do nosso "proxima faixa da
 * fila", o MA nao mandou SetNextAVTransportURI nenhum). Mandando
 * CurrentTrackURI/AVTransportURI (nova) e NextAVTransportURI (agora vazia)
 * ele enxerga a transicao e volta a alimentar a fila.
 *
 * Buffers estaticos de proposito: so a task de eventing (instancia unica)
 * passa por aqui, e as URIs do MA sao longas o bastante pra estourar a
 * pilha dela se fossem locais. */
static void dlna_build_lastchange_body(char *out, size_t out_size, const char *state_str,
                                       const char *cur_uri, const char *next_uri,
                                       const char *duration)
{
    char *inner = s_evt_inner;
    char *escaped = s_evt_esc;

    snprintf(inner, DLNA_BUF_EVT_INNER,
             "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
             "<InstanceID val=\"0\">"
             "<TransportState val=\"%s\"/>"
             "<CurrentTrackDuration val=\"%s\"/>"
             "<CurrentMediaDuration val=\"%s\"/>"
             "<CurrentTrackURI val=\"%s\"/>"
             "<AVTransportURI val=\"%s\"/>"
             "<NextAVTransportURI val=\"%s\"/>"
             "</InstanceID>"
             "</Event>",
             state_str, duration, duration, cur_uri, cur_uri, next_uri);
    xml_escape_into(escaped, DLNA_BUF_EVT_ESC, inner);
    snprintf(out, out_size,
             "<?xml version=\"1.0\"?>"
             "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
             "<e:property><LastChange>%s</LastChange></e:property>"
             "</e:propertyset>",
             escaped);
}

/* Task PERSISTENTE (criada uma unica vez em dlna_engine_init, mesma logica
 * das outras duas tasks do DLNA) -- dorme em xSemaphoreTake ate alguem
 * chamar dlna_notify_state_change_async(), ai manda um NOTIFY HTTP pro
 * callback URL que o control point deu no SUBSCRIBE. Fora da task do httpd
 * de proposito: e uma chamada de rede de SAIDA (pro control point, nao
 * resposta a uma requisicao dele), que pode demorar/travar -- nao pode
 * rodar dentro do handler SOAP original sem arriscar travar o servidor
 * inteiro pros outros control points. */
static void dlna_event_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_event_notify_sem, portMAX_DELAY);

        char callback[128];
        char sid[48];
        uint32_t seq;
        dlna_transport_state_t state;
        bool subscribed;
        char cur_uri[256], next_uri[256], duration[16];
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        subscribed = s_event_subscribed;
        strlcpy(callback, s_event_callback_url, sizeof(callback));
        strlcpy(sid, s_event_sid, sizeof(sid));
        seq = s_event_seq++;
        state = s_transport_state;
        strlcpy(cur_uri, s_current_uri, sizeof(cur_uri));
        strlcpy(next_uri, s_next_uri, sizeof(next_uri));
        strlcpy(duration, s_track_duration, sizeof(duration));
        xSemaphoreGive(s_state_mutex);

        if (!subscribed || callback[0] == '\0') {
            continue;
        }

        const char *state_str = state == DLNA_STATE_PLAYING     ? "PLAYING"
                                 : state == DLNA_STATE_PAUSED    ? "PAUSED_PLAYBACK"
                                                                  : "STOPPED";
        char *body = s_evt_body;
        dlna_build_lastchange_body(body, DLNA_BUF_EVT_BODY, state_str, cur_uri, next_uri, duration);
        char seq_str[16];
        snprintf(seq_str, sizeof(seq_str), "%" PRIu32, seq);

        esp_http_client_config_t config = {
            .url = callback,
            .method = HTTP_METHOD_NOTIFY,
            .timeout_ms = 3000, /* nunca pode travar essa task pra sempre */
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            continue;
        }
        esp_http_client_set_header(client, "NT", "upnp:event");
        esp_http_client_set_header(client, "NTS", "upnp:propchange");
        esp_http_client_set_header(client, "SID", sid);
        esp_http_client_set_header(client, "SEQ", seq_str);
        esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            /* Best-effort de proposito -- sem retry (o proximo evento de
             * estado real vai tentar de novo sozinho) e sem log em nivel de
             * erro (control point pode cancelar a assinatura/reiniciar sem
             * mandar UNSUBSCRIBE, isso e normal e nao indica bug daqui). */
            logger_log(ESP_LOG_WARN, TAG, "dlna: NOTIFY de evento falhou (%s)", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }
}

/* SUBSCRIBE/UNSUBSCRIBE (GENA) pro eventSubURL do AVTransport. So guarda
 * UMA assinatura (o unico control point real deste projeto e o Music
 * Assistant) -- uma segunda assinatura simplesmente substitui a anterior. */
static esp_err_t avtransport_event_handler(httpd_req_t *req)
{
    if (req->method == HTTP_SUBSCRIBE) {
        char sid[48] = "";
        bool renewal = httpd_req_get_hdr_value_str(req, "SID", sid, sizeof(sid)) == ESP_OK;

        if (renewal) {
            /* CRITICO (achado ao vivo, 2026-08-16): renovacao de um SID que
             * nao conhecemos -- caso classico depois de um reboot do
             * dispositivo, em que o control point ainda guarda o SID antigo
             * mas nos perdemos o callback URL junto com a RAM. Antes daqui,
             * respondiamos 200 OK e o control point ficava convencido de que
             * estava inscrito enquanto nunca mais recebia um NOTIFY -- com
             * isso ele nao era avisado do fim de faixa e a FILA TRAVAVA
             * (sintoma reportado: "sem flow a playlist nao prossegue").
             * A spec UPnP manda responder 412 pra SID desconhecido; ai o
             * control point descarta e refaz a assinatura completa (com
             * CALLBACK), que e exatamente o que precisamos. */
            bool known;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            known = s_event_subscribed && s_event_callback_url[0] != '\0' &&
                    strcmp(sid, s_event_sid) == 0;
            xSemaphoreGive(s_state_mutex);
            if (!known) {
                logger_log(ESP_LOG_WARN, TAG,
                           "dlna: renovacao de assinatura desconhecida (%s) -- 412 pra forcar nova", sid);
                httpd_resp_set_status(req, "412 Precondition Failed");
                httpd_resp_send(req, NULL, 0);
                return ESP_OK;
            }
        }

        if (!renewal) {
            char cb_raw[160] = "";
            char callback[128] = "";
            if (httpd_req_get_hdr_value_str(req, "CALLBACK", cb_raw, sizeof(cb_raw)) == ESP_OK) {
                /* CALLBACK vem como "<http://ip:porta/caminho>", as vezes
                 * com mais de uma URL -- so a primeira, entre os primeiros
                 * '<' '>' , interessa aqui. */
                char *start = strchr(cb_raw, '<');
                char *end = start ? strchr(start, '>') : NULL;
                if (start && end && end > start + 1) {
                    size_t cb_len = (size_t)(end - start - 1);
                    if (cb_len >= sizeof(callback)) {
                        cb_len = sizeof(callback) - 1;
                    }
                    memcpy(callback, start + 1, cb_len);
                    callback[cb_len] = '\0';
                }
            }

            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            strlcpy(s_event_callback_url, callback, sizeof(s_event_callback_url));
            snprintf(s_event_sid, sizeof(s_event_sid), "uuid:evt-%08" PRIx32, esp_random());
            s_event_seq = 0;
            s_event_subscribed = (callback[0] != '\0');
            strlcpy(sid, s_event_sid, sizeof(sid));
            xSemaphoreGive(s_state_mutex);

            logger_log(ESP_LOG_INFO, TAG, "dlna: eventing AVTransport assinado (callback=%s)",
                       callback[0] ? callback : "(vazio, ignorado)");
        }

        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_hdr(req, "SID", sid);
        httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, NULL, 0);

        /* UPnP GENA exige mandar o estado atual logo apos a assinatura,
         * nao so em mudancas futuras -- senao o control point comeca sem
         * saber se esta tocando ou nao ate a proxima mudanca de verdade. */
        dlna_notify_state_change_async();
    } else if (req->method == HTTP_UNSUBSCRIBE) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_event_subscribed = false;
        xSemaphoreGive(s_state_mutex);
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_send(req, NULL, 0);
    } else {
        soap_fault(req);
    }
    return ESP_OK;
}

/* RenderingControl/ConnectionManager: so confirma a assinatura (200 + SID)
 * pra nao gerar erro/retry no control point -- este firmware nao gera
 * eventos reais de volume/mute, so o AVTransport tem estado que muda o
 * suficiente pra valer o NOTIFY de verdade. */
static esp_err_t event_ack_only_handler(httpd_req_t *req)
{
    if (req->method == HTTP_SUBSCRIBE) {
        char sid[48];
        snprintf(sid, sizeof(sid), "uuid:evt-%08" PRIx32, esp_random());
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_hdr(req, "SID", sid);
        httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
        httpd_resp_send(req, NULL, 0);
    } else if (req->method == HTTP_UNSUBSCRIBE) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_send(req, NULL, 0);
    } else {
        soap_fault(req);
    }
    return ESP_OK;
}

/* Extrai titulo/artista/album/duracao do DIDL-Lite (XML escapado dentro de
 * outro XML) que vem em <CurrentURIMetaData> ou <NextURIMetaData> -- os dois
 * tem exatamente o mesmo formato interno, so muda a tag externa. */
static void dlna_parse_didl(const char *body, const char *tag,
                            char *track, size_t track_size,
                            char *artist, size_t artist_size,
                            char *album, size_t album_size,
                            char *duration, size_t duration_size)
{
    track[0] = '\0';
    artist[0] = '\0';
    album[0] = '\0';
    strlcpy(duration, "00:00:00", duration_size);

    char metadata[1024];
    if (!extract_xml_tag(body, tag, metadata, sizeof(metadata))) {
        return;
    }
    xml_unescape(metadata);
    extract_xml_tag(metadata, "dc:title", track, track_size);
    extract_xml_tag(metadata, "upnp:artist", artist, artist_size);
    extract_xml_tag(metadata, "upnp:album", album, album_size);
    /* Segundo nivel de desescape: o xml_unescape acima desfaz o escape do
     * ENVELOPE (o DIDL vem escapado dentro do XML do SOAP), mas o conteudo
     * das tags ainda traz as entidades proprias do DIDL. Sem isso, um titulo
     * como "Rock & Roll" chega aqui literalmente como "Rock &amp; Roll" e
     * era (a) exibido assim na pagina/MQTT e (b) escapado de novo ao
     * reemitir em GetMediaInfo/GetPositionInfo, virando "&amp;amp;".
     * Flagrado pelo auto-teste SOAP (scratchpad/test_soap.py). */
    xml_unescape(track);
    xml_unescape(artist);
    xml_unescape(album);
    /* Depois do decode, os campos podem ter multi-byte -- garante que nenhum
     * corte por tamanho deixou uma sequencia UTF-8 pela metade. */
    utf8_trim_incomplete(track);
    utf8_trim_incomplete(artist);
    utf8_trim_incomplete(album);

    /* duration="H:MM:SS[.mmm]" e um atributo do <res>, nao uma tag. VALIDA
     * antes de aceitar: ja veio "-1:58:12" (negativo!) do proprio Music
     * Assistant num stream de flow, e repassar isso pro control point/pagina
     * e pior que dizer "desconhecido". Tambem protege contra lixo vindo de
     * um DIDL-Lite truncado (metadata[] tem tamanho fixo -- capa de album
     * grande pode estourar e cortar no meio de um atributo). */
    char raw[16] = "";
    if (extract_xml_attr(metadata, "duration", raw, sizeof(raw))) {
        int h = -1, m = -1, s = -1;
        if (sscanf(raw, "%d:%d:%d", &h, &m, &s) == 3 &&
            h >= 0 && m >= 0 && m < 60 && s >= 0 && s < 60) {
            strlcpy(duration, raw, duration_size);
        } else {
            logger_log(ESP_LOG_WARN, TAG, "dlna: duracao invalida no DIDL ('%s'), tratando como desconhecida", raw);
        }
    }
}

static esp_err_t avtransport_control_handler(httpd_req_t *req)
{
    char header_buf[160];
    const char *action = soap_action_name(req, header_buf, sizeof(header_buf));
    /* 2048, nao 512 -- SetAVTransportURI carrega o DIDL-Lite inteiro
     * (titulo/artista/album/etc) dentro do corpo SOAP, bem maior que os
     * outros actions deste handler. */
    char body[2048];
    if (recv_body(req, body, sizeof(body)) != ESP_OK || action == NULL) {
        soap_fault(req);
        return ESP_OK;
    }
    dlna_capture_client_ip(req);

    if (strcmp(action, "SetAVTransportURI") == 0) {
        char uri[256];
        bool got_uri = extract_xml_tag(body, "CurrentURI", uri, sizeof(uri));

        char track[64], artist[64], album[64], duration[16];
        dlna_parse_didl(body, "CurrentURIMetaData", track, sizeof(track), artist, sizeof(artist),
                        album, sizeof(album), duration, sizeof(duration));

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        bool became_paused_stale = false;
        if (got_uri) {
            bool uri_changed = strcmp(s_current_uri, uri) != 0;
            strlcpy(s_current_uri, uri, sizeof(s_current_uri));
            /* URI nova (mesmo repetida) = faixa "recem carregada" pro
             * transporte -- zera o cronometro ate o proximo Play comecar a
             * contar de verdade. */
            s_playback_elapsed_base_us = 0;
            if (uri_changed) {
                /* Uma pausa em andamento (s_fetch_paused) apontava pra
                 * conexao/faixa ANTIGA -- invalida (generation nova) pra
                 * evitar que um Play seguinte tente "retomar" a faixa
                 * errada. Ver dlna_engine_play()/dlna_wait_while_paused(). */
                s_target_generation++;
                if (s_fetch_paused) {
                    s_fetch_paused = false;
                    became_paused_stale = true;
                }
                s_transport_state = DLNA_STATE_STOPPED;
                s_playing = false;
                /* Faixa nova explicita invalida qualquer proxima que ja
                 * estivesse enfileirada (era a continuacao da faixa
                 * antiga, nao dessa). */
                s_next_uri[0] = '\0';
            }
        }
        strlcpy(s_track, track, sizeof(s_track));
        strlcpy(s_artist, artist, sizeof(s_artist));
        strlcpy(s_album, album, sizeof(s_album));
        strlcpy(s_track_duration, duration, sizeof(s_track_duration));
        xSemaphoreGive(s_state_mutex);
        if (became_paused_stale && s_fetch_resume_sem != NULL) {
            xSemaphoreGive(s_fetch_resume_sem);
        }

        logger_log(ESP_LOG_INFO, TAG, "SetAVTransportURI: %s (%s - %s - %s, duracao=%s)",
                   got_uri ? uri : "(mantido)", track, artist, album, duration);
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "SetAVTransportURI", NULL);
    } else if (strcmp(action, "SetNextAVTransportURI") == 0) {
        /* Enfileira a proxima faixa COM os metadados dela -- e isso que
         * mantem titulo/artista/album/duracao corretos a cada troca, em vez
         * do control point cair no flow continuo (ver comentario em
         * s_next_uri). */
        char uri[256] = "";
        extract_xml_tag(body, "NextURI", uri, sizeof(uri));

        char track[64], artist[64], album[64], duration[16];
        dlna_parse_didl(body, "NextURIMetaData", track, sizeof(track), artist, sizeof(artist),
                        album, sizeof(album), duration, sizeof(duration));

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        strlcpy(s_next_uri, uri, sizeof(s_next_uri));
        strlcpy(s_next_track, track, sizeof(s_next_track));
        strlcpy(s_next_artist, artist, sizeof(s_next_artist));
        strlcpy(s_next_album, album, sizeof(s_next_album));
        strlcpy(s_next_duration, duration, sizeof(s_next_duration));
        xSemaphoreGive(s_state_mutex);

        logger_log(ESP_LOG_INFO, TAG, "SetNextAVTransportURI: %s (%s - %s, duracao=%s)",
                   uri[0] ? "enfileirada" : "(fila limpa)", track, artist, duration);
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "SetNextAVTransportURI", NULL);
    } else if (strcmp(action, "Play") == 0) {
        dlna_engine_play();
        logger_log(ESP_LOG_INFO, TAG, "Play solicitado (uri=%s)", s_current_uri);
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "Play", NULL);
    } else if (strcmp(action, "Pause") == 0) {
        dlna_engine_pause();
        logger_log(ESP_LOG_INFO, TAG, "Pause solicitado (uri=%s)", s_current_uri);
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "Pause", NULL);
    } else if (strcmp(action, "Stop") == 0) {
        char uri_before_stop[256];
        strlcpy(uri_before_stop, s_current_uri, sizeof(uri_before_stop));
        dlna_engine_stop();
        logger_log(ESP_LOG_INFO, TAG, "Stop solicitado (uri era: %s)", uri_before_stop);
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "Stop", NULL);
    } else if (strcmp(action, "GetTransportInfo") == 0) {
        const char *state_str = s_transport_state == DLNA_STATE_PLAYING   ? "PLAYING"
                                 : s_transport_state == DLNA_STATE_PAUSED ? "PAUSED_PLAYBACK"
                                                                            : "STOPPED";
        char state_body[192];
        snprintf(state_body, sizeof(state_body),
                 "<CurrentTransportState>%s</CurrentTransportState>"
                 "<CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>",
                 state_str);
        logger_log(ESP_LOG_INFO, TAG, "GetTransportInfo -> %s", state_str);
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "GetTransportInfo", state_body);
    } else if (strcmp(action, "GetPositionInfo") == 0) {
        int64_t elapsed_us = s_playback_elapsed_base_us;
        if (s_transport_state == DLNA_STATE_PLAYING) {
            elapsed_us += esp_timer_get_time() - s_playback_start_us;
        }
        char rel_time[16];
        dlna_format_hms(elapsed_us, rel_time, sizeof(rel_time));
        char duration[16];
        dlna_effective_duration(elapsed_us, duration, sizeof(duration));

        /* TrackURI + TrackMetaData preenchidos de verdade (antes iam vazios):
         * e daqui que o control point descobre QUAL faixa esta tocando depois
         * que a fila avanca sozinha. */
        char cur_uri[256];
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        strlcpy(cur_uri, s_current_uri, sizeof(cur_uri));
        xSemaphoreGive(s_state_mutex);
        static char uri_e[300];
        xml_escape_into(uri_e, sizeof(uri_e), cur_uri);
        const char *didl_e = dlna_current_didl_escaped();

        logger_log(ESP_LOG_INFO, TAG, "GetPositionInfo -> %s / %s", rel_time, duration);

        char *pos_body = s_soap_body;
        snprintf(pos_body, DLNA_BUF_SOAP_BODY,
                 "<Track>1</Track><TrackDuration>%s</TrackDuration>"
                 "<TrackMetaData>%s</TrackMetaData><TrackURI>%s</TrackURI>"
                 "<RelTime>%s</RelTime><AbsTime>%s</AbsTime><RelCount>0</RelCount><AbsCount>0</AbsCount>",
                 duration, didl_e, uri_e, rel_time, rel_time);
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "GetPositionInfo", pos_body);
    } else if (strcmp(action, "GetMediaInfo") == 0) {
        /* Acao padrao do AVTransport que faltava por completo -- e a que o
         * control point usa pra perguntar "o que esta carregado agora?".
         * Sem ela ele so tinha o que mandou no ultimo SetAVTransportURI, e
         * por isso ficava preso na primeira musica da fila. */
        char cur_uri[256], next_uri[256], duration[16];
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        strlcpy(cur_uri, s_current_uri, sizeof(cur_uri));
        strlcpy(next_uri, s_next_uri, sizeof(next_uri));
        strlcpy(duration, s_track_duration, sizeof(duration));
        xSemaphoreGive(s_state_mutex);

        static char cur_e[300], next_e[300];
        xml_escape_into(cur_e, sizeof(cur_e), cur_uri);
        xml_escape_into(next_e, sizeof(next_e), next_uri);
        const char *didl_e = dlna_current_didl_escaped();

        char *media_body = s_soap_body;
        snprintf(media_body, DLNA_BUF_SOAP_BODY,
                 "<NrTracks>1</NrTracks><MediaDuration>%s</MediaDuration>"
                 "<CurrentURI>%s</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>"
                 "<NextURI>%s</NextURI><NextURIMetaData></NextURIMetaData>"
                 "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                 "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
                 duration, cur_e, didl_e, next_e);
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "GetMediaInfo", media_body);
    } else {
        soap_fault(req);
    }
    return ESP_OK;
}

static esp_err_t renderingcontrol_control_handler(httpd_req_t *req)
{
    char header_buf[160];
    const char *action = soap_action_name(req, header_buf, sizeof(header_buf));
    char body[512];
    if (recv_body(req, body, sizeof(body)) != ESP_OK || action == NULL) {
        soap_fault(req);
        return ESP_OK;
    }

    if (strcmp(action, "SetVolume") == 0) {
        char val[16];
        if (extract_xml_tag(body, "DesiredVolume", val, sizeof(val))) {
            int upnp_vol = atoi(val); /* 0-100 */
            audio_codec_set_volume((upnp_vol * VOLUME_STEPS) / 100);
        }
        soap_respond(req, "urn:schemas-upnp-org:service:RenderingControl:1", "SetVolume", NULL);
    } else if (strcmp(action, "GetVolume") == 0) {
        int upnp_vol = (audio_codec_get_volume() * 100) / VOLUME_STEPS;
        char body_resp[64];
        snprintf(body_resp, sizeof(body_resp), "<CurrentVolume>%d</CurrentVolume>", upnp_vol);
        soap_respond(req, "urn:schemas-upnp-org:service:RenderingControl:1", "GetVolume", body_resp);
    } else if (strcmp(action, "SetMute") == 0) {
        char val[8];
        if (extract_xml_tag(body, "DesiredMute", val, sizeof(val))) {
            audio_codec_set_mute(strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
        }
        soap_respond(req, "urn:schemas-upnp-org:service:RenderingControl:1", "SetMute", NULL);
    } else {
        soap_fault(req);
    }
    return ESP_OK;
}

static esp_err_t connectionmanager_control_handler(httpd_req_t *req)
{
    char header_buf[160];
    const char *action = soap_action_name(req, header_buf, sizeof(header_buf));
    char body[128];
    recv_body(req, body, sizeof(body)); /* corpo nao usado por essas actions, so drena */
    if (action == NULL) {
        soap_fault(req);
        return ESP_OK;
    }

    if (strcmp(action, "GetProtocolInfo") == 0) {
        /* audio/flac adicionado -- reaproveita o decoder nativo ja mantido
         * neste firmware (flac_stream_decoder.h). WAV/L16 continuam
         * anunciados (suportados via dlna_stream_pcm, sem decoder algum). */
        soap_respond(req, "urn:schemas-upnp-org:service:ConnectionManager:1", "GetProtocolInfo",
                     "<Source></Source>"
                     "<Sink>http-get:*:audio/wav:*,http-get:*:audio/L16;rate=44100;channels=2:*,"
                     "http-get:*:audio/flac:*,http-get:*:audio/x-flac:*</Sink>");
    } else if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
        soap_respond(req, "urn:schemas-upnp-org:service:ConnectionManager:1", "GetCurrentConnectionIDs",
                     "<ConnectionIDs>0</ConnectionIDs>");
    } else if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
        soap_respond(req, "urn:schemas-upnp-org:service:ConnectionManager:1", "GetCurrentConnectionInfo",
                     "<RcsID>-1</RcsID><AVTransportID>-1</AVTransportID><ProtocolInfo></ProtocolInfo>"
                     "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
                     "<Direction>Input</Direction><Status>OK</Status>");
    } else {
        soap_fault(req);
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * SSDP (descoberta) -- socket UDP multicast proprio, task dedicada.
 * ------------------------------------------------------------------------- */

static void ssdp_send_notify_alive(int sock, const struct sockaddr_in *mcast_dest, const char *ip_str)
{
    static const char *nts_list[] = {
        "upnp:rootdevice",
        NULL, /* uuid puro -- preenchido abaixo */
        "urn:schemas-upnp-org:device:MediaRenderer:1",
        "urn:schemas-upnp-org:service:AVTransport:1",
        "urn:schemas-upnp-org:service:RenderingControl:1",
        "urn:schemas-upnp-org:service:ConnectionManager:1",
    };

    for (size_t i = 0; i < sizeof(nts_list) / sizeof(nts_list[0]); i++) {
        const char *nt = nts_list[i] ? nts_list[i] : s_uuid;
        char usn[192];
        if (nts_list[i] == NULL) {
            snprintf(usn, sizeof(usn), "%s", s_uuid);
        } else {
            snprintf(usn, sizeof(usn), "%s::%s", s_uuid, nt);
        }

        char msg[768]; /* folga extra pro -Wformat-truncation (ver description_xml_handler) */
        int len = snprintf(msg, sizeof(msg),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "CACHE-CONTROL: max-age=1800\r\n"
            "LOCATION: http://%s:%d/description.xml\r\n"
            "NT: %s\r\n"
            "NTS: ssdp:alive\r\n"
            "SERVER: ESP32 UPnP/1.0 ReceiverBT/1.0\r\n"
            "USN: %s\r\n"
            "\r\n",
            ip_str, DLNA_HTTP_PORT, nt, usn);
        sendto(sock, msg, len, 0, (const struct sockaddr *)mcast_dest, sizeof(*mcast_dest));
        /* Pequeno intervalo entre os anuncios -- mandar os 6 em rajada sem
         * pausa arrisca perda por colisao no Wi-Fi (dispositivos DLNA reais
         * tambem espacam esses anuncios; um roteador/AP sobrecarregado
         * pode descartar parte de uma rajada instantanea). */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Acha o VALOR de um cabecalho HTTP/SSDP pelo nome, exigindo que o nome
 * comece no inicio de uma linha (nao em qualquer lugar do texto). BUG REAL
 * caçado com tcpdump de verdade (Music Assistant nunca buscava o
 * description.xml, mesmo o M-SEARCH e nossa resposta chegando certinhos
 * pela rede -- nao era problema de multicast/Docker como se suspeitava):
 * strstr(req, "ST:") solto encontra a substring "ST:" ESCONDIDA DENTRO de
 * "HOST:" (HO-ST:), que sempre vem ANTES do cabeçalho "ST:" de verdade
 * numa requisicao M-SEARCH real -- entao a resposta saia com o endereco do
 * HOST (ex.: "239.255.255.250:1900") no lugar do tipo de dispositivo
 * pedido, e qualquer control point UPnP descartava a resposta por nao
 * reconhecer esse "ST" sem sentido. */
static const char *find_header_value(const char *req, const char *name)
{
    size_t name_len = strlen(name);
    const char *p = req;
    while (p != NULL && *p != '\0') {
        if (strncasecmp(p, name, name_len) == 0) {
            return p + name_len;
        }
        p = strchr(p, '\n');
        if (p != NULL) {
            p++; /* pula o '\n', aponta pro comeco da proxima linha */
        }
    }
    return NULL;
}

static void ssdp_handle_msearch(int sock, const char *req, const struct sockaddr_in *from_addr,
                                 socklen_t from_len, const char *ip_str)
{
    char st[128] = "upnp:rootdevice";
    const char *st_line = find_header_value(req, "ST:");
    if (st_line) {
        while (*st_line == ' ') {
            st_line++;
        }
        size_t i = 0;
        while (st_line[i] && st_line[i] != '\r' && st_line[i] != '\n' && i < sizeof(st) - 1) {
            st[i] = st_line[i];
            i++;
        }
        st[i] = '\0';
    }

    char usn[192];
    if (strcmp(st, s_uuid) == 0) {
        snprintf(usn, sizeof(usn), "%s", s_uuid);
    } else {
        snprintf(usn, sizeof(usn), "%s::%s", s_uuid, st);
    }

    char resp[768]; /* folga extra pro -Wformat-truncation (ver description_xml_handler) */
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s:%d/description.xml\r\n"
        "SERVER: ESP32 UPnP/1.0 ReceiverBT/1.0\r\n"
        "ST: %s\r\n"
        "USN: %s\r\n"
        "\r\n",
        ip_str, DLNA_HTTP_PORT, st, usn);
    sendto(sock, resp, len, 0, (const struct sockaddr *)from_addr, from_len);
}

static void ssdp_task(void *arg)
{
    /* Espera o Wi-Fi ter IP valido ANTES de criar/configurar o socket --
     * precisamos do IP local pra fixar a interface do multicast (ver
     * abaixo), nao da pra fazer isso com INADDR_ANY de forma confiavel. */
    char ip_str[16];
    for (;;) {
        wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
        if (ip_str[0] != '\0') {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    struct in_addr local_if = {.s_addr = inet_addr(ip_str)};

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "falha ao criar socket SSDP");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SSDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        ESP_LOGE(TAG, "falha ao dar bind na porta SSDP (%d)", SSDP_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    /* IP_MULTICAST_IF + imr_interface com o IP real da STA (nao INADDR_ANY):
     * em lwIP isso e o que garante que TX/RX de multicast usem a interface
     * Wi-Fi de verdade -- INADDR_ANY como "deixa o stack escolher" e
     * ambiguo o suficiente pra as vezes nao funcionar direito, mesmo com
     * uma unica interface ativa. E a causa mais provavel do SSDP nao
     * aparecer nem no Explorador de Rede do Windows apesar do HTTP do
     * DLNA funcionar normalmente por IP direto. */
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &local_if, sizeof(local_if));

    struct ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_MCAST_ADDR);
    mreq.imr_interface = local_if;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGW(TAG, "falha ao entrar no grupo multicast SSDP");
    }

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in mcast_dest = {
        .sin_family = AF_INET,
        .sin_port = htons(SSDP_PORT),
    };
    mcast_dest.sin_addr.s_addr = inet_addr(SSDP_MCAST_ADDR);

    /* Manda o alive JA no boot, nao so depois do primeiro intervalo de
     * SSDP_NOTIFY_INTERVAL_S (60s) -- antes disso, um control point (ex.:
     * Music Assistant) que ja tinha o dispositivo cadastrado de uma sessao
     * anterior so descobria ele de novo depois de ate 1 minuto (ou nunca,
     * se a entrada dele ja tivesse expirado do lado do MA e nada tivesse
     * reanunciado a tempo) -- reportado ao vivo: precisava reiniciar a
     * integracao DLNA no MA manualmente a cada reflash. Manda 2x (a
     * especificacao UPnP recomenda mais de um envio no boot, ja que SSDP e
     * UDP/multicast, sem garantia de entrega). */
    ssdp_send_notify_alive(sock, &mcast_dest, ip_str);
    vTaskDelay(pdMS_TO_TICKS(500));
    ssdp_send_notify_alive(sock, &mcast_dest, ip_str);
    int64_t last_notify_s = esp_timer_get_time() / 1000000;
    char buf[512];

    for (;;) {
        wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
        if (ip_str[0] == '\0') {
            /* Wi-Fi caiu -- so espera reconectar, mantem socket/join como esta */
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int64_t now_s = esp_timer_get_time() / 1000000;
        if (now_s - last_notify_s >= SSDP_NOTIFY_INTERVAL_S) {
            ssdp_send_notify_alive(sock, &mcast_dest, ip_str);
            last_notify_s = now_s;
        }

        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from_addr, &from_len);
        if (len <= 0) {
            continue; /* timeout do SO_RCVTIMEO -- volta e rechecka o NOTIFY periodico */
        }
        buf[len] = '\0';

        if (strncmp(buf, "M-SEARCH", 8) == 0) {
            ssdp_handle_msearch(sock, buf, &from_addr, from_len, ip_str);
        }
    }
}

/* -------------------------------------------------------------------------
 * Inicializacao
 * ------------------------------------------------------------------------- */

static void build_uuid(void)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_uuid, sizeof(s_uuid), "uuid:4d696e69-4d65-6469-6161-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Aloca o ring buffer/semaforo/mutex e sobe as duas tasks persistentes
 * (busca+decode, I2S) uma unica vez no boot -- mesmo motivo do
 * bt_audio_prealloc_ring_buffer(): alocar sob demanda, so quando a primeira
 * faixa DLNA chegasse, concorreria com WiFi/HTTP/mDNS ja fragmentando o
 * heap depois do boot. */
static void dlna_engine_init(void)
{
    /* XML pesado vai pra PSRAM (ver comentario nos #define DLNA_BUF_*). */
    s_soap_resp = heap_caps_malloc(DLNA_BUF_SOAP_RESP, MALLOC_CAP_SPIRAM);
    s_soap_body = heap_caps_malloc(DLNA_BUF_SOAP_BODY, MALLOC_CAP_SPIRAM);
    s_didl_esc = heap_caps_malloc(DLNA_BUF_DIDL_ESC, MALLOC_CAP_SPIRAM);
    s_didl_raw = heap_caps_malloc(DLNA_BUF_DIDL_RAW, MALLOC_CAP_SPIRAM);
    s_evt_inner = heap_caps_malloc(DLNA_BUF_EVT_INNER, MALLOC_CAP_SPIRAM);
    s_evt_esc = heap_caps_malloc(DLNA_BUF_EVT_ESC, MALLOC_CAP_SPIRAM);
    s_evt_body = heap_caps_malloc(DLNA_BUF_EVT_BODY, MALLOC_CAP_SPIRAM);
    s_xml_bufs_ok = s_soap_resp && s_soap_body && s_didl_esc && s_didl_raw &&
                    s_evt_inner && s_evt_esc && s_evt_body;
    if (!s_xml_bufs_ok) {
        ESP_LOGE(TAG, "dlna: sem PSRAM pros buffers de XML");
    }

    s_state_mutex = xSemaphoreCreateMutex();
    s_i2s_write_sem = xSemaphoreCreateBinary();
    s_event_notify_sem = xSemaphoreCreateBinary();
    s_fetch_resume_sem = xSemaphoreCreateBinary();
    if (s_ringbuf_storage == NULL) {
        s_ringbuf_storage = heap_caps_malloc(DLNA_RINGBUF_HIGHEST_WATER_LEVEL, MALLOC_CAP_SPIRAM);
    }
    if (s_ringbuf_storage != NULL) {
        s_ringbuf = xRingbufferCreateStatic(DLNA_RINGBUF_HIGHEST_WATER_LEVEL, RINGBUF_TYPE_BYTEBUF,
                                             s_ringbuf_storage, &s_ringbuf_struct);
    }
    if (s_state_mutex == NULL || s_i2s_write_sem == NULL || s_event_notify_sem == NULL ||
        s_fetch_resume_sem == NULL || s_ringbuf == NULL) {
        ESP_LOGE(TAG, "dlna: falha ao pre-alocar buffer/semaforo/mutex -- sem audio DLNA nesta sessao");
        return;
    }

    if (xTaskCreate(dlna_i2s_task_handler, "dlna_i2s", 2560, NULL, configMAX_PRIORITIES - 3,
                     &s_i2s_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "dlna: falha ao criar task de I2S");
    }
    if (xTaskCreate(dlna_fetch_task, "dlna_fetch", DLNA_FETCH_TASK_STACK, NULL, 5,
                     &s_fetch_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "dlna: falha ao criar task de busca/decode");
    }
    /* Stack modesta -- so monta um XML pequeno e faz uma chamada HTTP de
     * saida via esp_http_client, nada perto do que a task de FLAC precisa. */
    if (xTaskCreate(dlna_event_task, "dlna_event", 4096, NULL, 4,
                     &s_event_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "dlna: falha ao criar task de eventing");
    }
}

void dlna_renderer_get_status(dlna_status_t *out)
{
    if (out == NULL || s_state_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    out->playing = s_playing;
    strlcpy(out->track, s_track, sizeof(out->track));
    strlcpy(out->artist, s_artist, sizeof(out->artist));
    strlcpy(out->album, s_album, sizeof(out->album));
    strlcpy(out->client_ip, s_client_ip, sizeof(out->client_ip));
    strlcpy(out->client_agent, s_client_agent, sizeof(out->client_agent));
    strlcpy(out->state, s_transport_state == DLNA_STATE_PLAYING   ? "playing"
                        : s_transport_state == DLNA_STATE_PAUSED  ? "paused"
                                                                    : "stopped",
            sizeof(out->state));
    out->subscribed = s_event_subscribed;
    int64_t elapsed_us = s_playback_elapsed_base_us;
    if (s_transport_state == DLNA_STATE_PLAYING) {
        elapsed_us += esp_timer_get_time() - s_playback_start_us;
    }
    dlna_format_hms(elapsed_us, out->position, sizeof(out->position));
    dlna_effective_duration(elapsed_us, out->duration, sizeof(out->duration));
    xSemaphoreGive(s_state_mutex);
}

esp_err_t dlna_renderer_media_control(const char *cmd)
{
    if (cmd == NULL || s_state_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Pular faixa nao existe do lado do renderer: a fila e do control point.
     * Melhor devolver "nao suportado" e deixar a interface avisar do que
     * fingir que funcionou (era o que acontecia -- o comando ia so pro
     * bt_audio e sumia silenciosamente quando a fonte era DLNA). */
    if (strcmp(cmd, "next") == 0 || strcmp(cmd, "previous") == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool have_uri = (s_current_uri[0] != '\0');
    dlna_transport_state_t state = s_transport_state;
    xSemaphoreGive(s_state_mutex);

    if (strcmp(cmd, "stop") == 0) {
        if (!have_uri && state == DLNA_STATE_STOPPED) {
            return ESP_ERR_INVALID_STATE;
        }
        dlna_engine_stop();
        return ESP_OK;
    }
    if (strcmp(cmd, "pause") == 0) {
        if (state != DLNA_STATE_PLAYING) {
            return ESP_ERR_INVALID_STATE;
        }
        dlna_engine_pause();
        return ESP_OK;
    }
    if (strcmp(cmd, "play") == 0) {
        if (!have_uri) {
            return ESP_ERR_INVALID_STATE; /* nada carregado pra tocar */
        }
        dlna_engine_play();
        return ESP_OK;
    }
    if (strcmp(cmd, "playpause") == 0) {
        if (state == DLNA_STATE_PLAYING) {
            dlna_engine_pause();
            return ESP_OK;
        }
        if (!have_uri) {
            return ESP_ERR_INVALID_STATE;
        }
        dlna_engine_play();
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

void dlna_renderer_init(void)
{
    dlna_engine_init();
    build_uuid();
    if (storage_get_str(NVS_KEY_DEVICE_NAME, s_friendly_name, sizeof(s_friendly_name)) != ESP_OK) {
        strlcpy(s_friendly_name, FW_DEVICE_NAME_DEFAULT, sizeof(s_friendly_name));
    }

    if (!s_xml_bufs_ok) {
        /* Sem os buffers de XML nao da pra responder SOAP nenhum -- melhor
         * nem subir o servidor do que servir respostas truncadas/corrompidas
         * (ou escrever em ponteiro nulo) pro control point. */
        ESP_LOGE(TAG, "dlna: servidor DLNA nao iniciado (buffers de XML indisponiveis)");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = DLNA_HTTP_PORT;
    config.ctrl_port = DLNA_HTTP_CTRL_PORT;
    config.max_uri_handlers = 16; /* 7 originais + 4 de SUBSCRIBE/UNSUBSCRIBE (eventing) + folga */
    /* CRITICO (achado ao vivo, 2026-08-16, via "stack overflow in task
     * httpd" real capturado no monitor serial): o default do ESP-IDF
     * (~4096 bytes) nao sobra margem suficiente pra avtransport_control_handler,
     * que agora usa buffers bem maiores na propria pilha (body[2048] +
     * metadata[1024] + campos de metadados) pra processar o DIDL-Lite do
     * SetAVTransportURI. O crash "misterioso" que parecia vir das tasks
     * novas de audio (I2S/busca) era na verdade isso -- confirmado
     * isolando as duas tasks (ficaram estaveis sozinhas) e so depois
     * capturando o "stack overflow in task httpd" de verdade. */
    config.stack_size = 8192;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "falha ao iniciar o servidor HTTP do DLNA (porta %d)", DLNA_HTTP_PORT);
        return;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/description.xml", .method = HTTP_GET, .handler = description_xml_handler},
        {.uri = "/AVTransport.xml", .method = HTTP_GET, .handler = scpd_handler, .user_ctx = (void *)AVTRANSPORT_SCPD},
        {.uri = "/RenderingControl.xml",
         .method = HTTP_GET,
         .handler = scpd_handler,
         .user_ctx = (void *)RENDERINGCONTROL_SCPD},
        {.uri = "/ConnectionManager.xml",
         .method = HTTP_GET,
         .handler = scpd_handler,
         .user_ctx = (void *)CONNECTIONMANAGER_SCPD},
        {.uri = "/AVTransport/control", .method = HTTP_POST, .handler = avtransport_control_handler},
        {.uri = "/RenderingControl/control", .method = HTTP_POST, .handler = renderingcontrol_control_handler},
        {.uri = "/ConnectionManager/control", .method = HTTP_POST, .handler = connectionmanager_control_handler},
        /* Eventing (GENA) -- sem isso o control point (Music Assistant) fica
         * sem saber de mudancas de estado (Play/Pause/Stop) em tempo real,
         * so descobriria sondando por conta propria (achado ao vivo:
         * status ficava "tocando" mesmo depois de pausar de verdade). */
        {.uri = "/AVTransport/event", .method = HTTP_SUBSCRIBE, .handler = avtransport_event_handler},
        {.uri = "/AVTransport/event", .method = HTTP_UNSUBSCRIBE, .handler = avtransport_event_handler},
        {.uri = "/RenderingControl/event", .method = HTTP_SUBSCRIBE, .handler = event_ack_only_handler},
        {.uri = "/RenderingControl/event", .method = HTTP_UNSUBSCRIBE, .handler = event_ack_only_handler},
        {.uri = "/ConnectionManager/event", .method = HTTP_SUBSCRIBE, .handler = event_ack_only_handler},
        {.uri = "/ConnectionManager/event", .method = HTTP_UNSUBSCRIBE, .handler = event_ack_only_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_httpd, &routes[i]);
    }

    xTaskCreate(ssdp_task, "ssdp_task", 4096, NULL, 4, NULL);

    logger_log(ESP_LOG_INFO, TAG, "DLNA MediaRenderer pronto: \"%s\" (%s), HTTP na porta %d",
               s_friendly_name, s_uuid, DLNA_HTTP_PORT);
}
