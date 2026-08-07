#pragma once

/* A2DP sink + AVRCP (Controller) via Bluedroid nativo do ESP-IDF.
 * Inicializa o controlador BT, o Bluedroid, GAP (pareamento Secure Simple
 * Pairing "Just Works"), A2DP sink e AVRCP CT (metadados/track change).
 * O áudio recebido é decodificado (SBC -> PCM) pela própria pilha Bluedroid
 * e encaminhado para audio_codec_write(). */

void bt_audio_init(void);
