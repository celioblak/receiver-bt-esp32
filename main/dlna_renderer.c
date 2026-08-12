#include "dlna_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_codec.h"
#include "config.h"
#include "logger.h"
#include "storage.h"
#include "wifi_manager.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
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

/* Estado do AVTransport -- marco 1: so guarda o que foi pedido e responde
 * certo, ainda nao busca o audio de fato (ver dlna_renderer.h). */
static char s_current_uri[256] = "";
static volatile bool s_playing = false;

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
    "<action><name>Pause</name></action>"
    "<action><name>Stop</name></action>"
    "<action><name>GetTransportInfo</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>"
    "<argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>"
    "<argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetPositionInfo</name></action>"
    "</actionList>"
    "<serviceStateTable>"
    "<stateVariable sendEvents=\"no\"><name>TransportState</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>TransportStatus</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>TransportPlaySpeed</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>AVTransportURIMetaData</name><dataType>string</dataType></stateVariable>"
    "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_InstanceID</name><dataType>ui4</dataType></stateVariable>"
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
    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse></s:Body></s:Envelope>",
        action, service_type, body_extra ? body_extra : "", action);
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
    static const char PART2[] =
        "</friendlyName>"
        "<manufacturer>DIY</manufacturer>"
        "<modelName>ESP32 Audio Kit V2.2</modelName>"
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

static esp_err_t avtransport_control_handler(httpd_req_t *req)
{
    char header_buf[160];
    const char *action = soap_action_name(req, header_buf, sizeof(header_buf));
    char body[512];
    if (recv_body(req, body, sizeof(body)) != ESP_OK || action == NULL) {
        soap_fault(req);
        return ESP_OK;
    }

    if (strcmp(action, "SetAVTransportURI") == 0) {
        char uri[256];
        if (extract_xml_tag(body, "CurrentURI", uri, sizeof(uri))) {
            strlcpy(s_current_uri, uri, sizeof(s_current_uri));
            logger_log(ESP_LOG_INFO, TAG, "SetAVTransportURI: %s", s_current_uri);
        }
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "SetAVTransportURI", NULL);
    } else if (strcmp(action, "Play") == 0) {
        s_playing = true;
        logger_log(ESP_LOG_INFO, TAG, "Play solicitado (uri=%s) -- reproducao real ainda nao implementada (marco 1)",
                   s_current_uri);
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "Play", NULL);
    } else if (strcmp(action, "Pause") == 0) {
        s_playing = false;
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "Pause", NULL);
    } else if (strcmp(action, "Stop") == 0) {
        s_playing = false;
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "Stop", NULL);
    } else if (strcmp(action, "GetTransportInfo") == 0) {
        char state_body[192];
        snprintf(state_body, sizeof(state_body),
                 "<CurrentTransportState>%s</CurrentTransportState>"
                 "<CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>",
                 s_playing ? "PLAYING" : "STOPPED");
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "GetTransportInfo", state_body);
    } else if (strcmp(action, "GetPositionInfo") == 0) {
        soap_respond(req, "urn:schemas-upnp-org:service:AVTransport:1", "GetPositionInfo",
                     "<Track>1</Track><TrackDuration>00:00:00</TrackDuration><TrackURI></TrackURI>"
                     "<RelTime>00:00:00</RelTime><AbsTime>00:00:00</AbsTime><RelCount>0</RelCount><AbsCount>0</AbsCount>");
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
        soap_respond(req, "urn:schemas-upnp-org:service:ConnectionManager:1", "GetProtocolInfo",
                     "<Source></Source>"
                     "<Sink>http-get:*:audio/wav:*,http-get:*:audio/L16;rate=44100;channels=2:*</Sink>");
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

static void ssdp_handle_msearch(int sock, const char *req, const struct sockaddr_in *from_addr,
                                 socklen_t from_len, const char *ip_str)
{
    char st[128] = "upnp:rootdevice";
    const char *st_line = strstr(req, "ST:");
    if (st_line) {
        st_line += 3;
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

    int64_t last_notify_s = 0;
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

void dlna_renderer_init(void)
{
    build_uuid();
    if (storage_get_str(NVS_KEY_DEVICE_NAME, s_friendly_name, sizeof(s_friendly_name)) != ESP_OK) {
        strlcpy(s_friendly_name, FW_DEVICE_NAME_DEFAULT, sizeof(s_friendly_name));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = DLNA_HTTP_PORT;
    config.ctrl_port = DLNA_HTTP_CTRL_PORT;
    config.max_uri_handlers = 8;

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
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_httpd, &routes[i]);
    }

    xTaskCreate(ssdp_task, "ssdp_task", 4096, NULL, 4, NULL);

    logger_log(ESP_LOG_INFO, TAG, "DLNA MediaRenderer pronto: \"%s\" (%s), HTTP na porta %d",
               s_friendly_name, s_uuid, DLNA_HTTP_PORT);
}
