#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* DLNA/UPnP MediaRenderer -- permite que o Music Assistant (ou qualquer
 * control point DLNA: BubbleUPnP, VLC, etc.) descubra, "veja" e toque áudio
 * de verdade neste dispositivo, independente do Bluetooth.
 *
 * Confirmado lendo o fonte real do provider DLNA do Music Assistant: é um
 * control point (baseado no dlna_dmr do Home Assistant) que EMPURRA um URL
 * de stream pro renderer via SetAVTransportURI -- exatamente o papel deste
 * arquivo. SSDP (descoberta) + toda a superfície SOAP já funcionavam; agora
 * busca (HTTP GET via esp_http_client) e decodifica (WAV/PCM direto, FLAC
 * via flac_stream_decoder.h) o áudio de verdade, entregando pro codec pelo
 * mesmo padrão de ring buffer + task dedicada de I2S já comprovado em
 * bt_audio.c. BT sempre tem prioridade -- ver dlna_fetch_task em
 * dlna_renderer.c. */

void dlna_renderer_init(void);

typedef struct {
    bool playing;
    char track[64];
    char artist[64];
    char album[64];
    /* IP do ultimo control point DLNA que mandou uma acao AVTransport (ex.:
     * Music Assistant); vazio se nenhum ainda desde o boot. */
    char client_ip[16];
    /* User-Agent da mesma requisicao -- costuma identificar a biblioteca/app
     * do control point quando o IP sozinho nao diz "quem". Pode vir vazio se
     * o control point nao mandar esse cabecalho. */
    char client_agent[64];
    /* Estado real de transporte UPnP ("stopped"/"playing"/"paused") --
     * client_ip/client_agent ficam gravados desde a ULTIMA acao SOAP
     * (Play, Pause ou Stop), entao "playing" sozinho nao basta pra
     * pagina web mostrar coerentemente "conectado mas pausado" em vez de
     * "inativo" quando na verdade so foi pausado. */
    char state[16];
    /* Se o control point tem uma assinatura de eventing (GENA) ativa agora
     * -- proxy de "esta com o dispositivo adicionado", parecido com
     * bt_connected pro Bluetooth. */
    bool subscribed;
    /* Posicao/duracao reais da faixa atual, "H:MM:SS" -- ver
     * s_playback_elapsed_base_us em dlna_renderer.c. duration fica
     * "00:00:00" se o control point nao mandou essa informacao. */
    char position[16];
    char duration[16];
} dlna_status_t;

/* Cópia thread-safe do estado atual (usado por web_server.c/mqtt_ha.c). */
void dlna_renderer_get_status(dlna_status_t *out);

/* Controle de mídia da fonte DLNA, para a página web e o MQTT/Home Assistant
 * (o equivalente ao bt_audio_media_control() do lado Bluetooth).
 *
 * Aceita "play", "pause", "playpause" e "stop" -- todas operações locais
 * nossas. Devolve ESP_ERR_NOT_SUPPORTED para "next"/"previous": no DLNA a
 * fila pertence ao control point (Music Assistant) e o protocolo não dá ao
 * renderer nenhuma forma de pedir "próxima faixa" -- quem pula faixa é quem
 * manda o SetAVTransportURI. Para pular faixa fora da tela do MA, o caminho
 * é controlar o MA (integração nativa dele no Home Assistant).
 *
 * Devolve ESP_ERR_INVALID_STATE se não há nada carregado/tocando via DLNA. */
esp_err_t dlna_renderer_media_control(const char *cmd);
