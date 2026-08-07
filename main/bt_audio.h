#pragma once

#include <stdbool.h>

/* A2DP sink + AVRCP (Controller) via Bluedroid nativo do ESP-IDF.
 * Inicializa o controlador BT, o Bluedroid, GAP (pareamento Secure Simple
 * Pairing "Just Works"), A2DP sink e AVRCP CT (metadados/track change).
 * O áudio recebido é decodificado (SBC -> PCM) pela própria pilha Bluedroid
 * e encaminhado para audio_codec_write(). */

void bt_audio_init(void);

typedef struct {
    bool connected;
    bool playing;
    char remote_mac[18];
    char title[64];
    char artist[64];
    char album[64];
} bt_audio_status_t;

/* Cópia thread-safe do estado atual (usado por web_server.c em /api/status). */
void bt_audio_get_status(bt_audio_status_t *out);
