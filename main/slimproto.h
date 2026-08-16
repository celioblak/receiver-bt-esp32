#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Cliente Slimproto (protocolo do Squeezebox/Logitech Media Server) —
 * alternativa ao DLNA (ver dlna_renderer.h) para integrar com o Music
 * Assistant: em vez de depender de descoberta multicast (SSDP), o ESP32
 * conecta diretamente no IP do servidor (porta fixa 3483), do mesmo jeito
 * que já fazemos com o broker MQTT. Isso contorna todo o problema de
 * descoberta via Docker/multicast que impediu o DLNA de funcionar com o MA.
 *
 * Formato de áudio: negociamos só PCM (sem FLAC/MP3), então os bytes que
 * chegam vão direto para audio_codec_write(), sem decoder.
 *
 * Se o Bluetooth A2DP estiver conectado, o Slimproto cede a saída de áudio
 * (ver bt_audio_get_status() em slimproto.c) — BT sempre tem prioridade. */

typedef struct {
    bool enabled;      /* host configurado (não vazio) */
    bool connected;    /* conexão de controle com o servidor está de pé */
    bool playing;      /* stream de áudio ativo no momento */
    char server_host[64];
} slimproto_status_t;

void slimproto_init(void);

void slimproto_get_status(slimproto_status_t *out);

/* Instante (esp_timer_get_time(), microssegundos) da última transição de
 * sessão de dados (troca de faixa, natural ou via comando) -- usado por
 * lms_cli.c pra dar uma folga antes de abrir sua própria conexão TCP nova
 * se uma transição acabou de acontecer, reduzindo a janela de concorrência
 * entre as duas operações de socket disputando as estruturas internas do
 * lwIP ao mesmo tempo. Retorna 0 se nenhuma transição aconteceu ainda. */
int64_t slimproto_get_last_transition_time_us(void);
