#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

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
    /* Preenchido quando bt_audio_set_require_pin(true) e um dispositivo
     * novo esta no meio do pareamento (Passkey Entry) -- pending_pin_code
     * vazio ("") = nenhum pareamento pendente. A pessoa digita esse codigo
     * no celular pra completar o pareamento. */
    char pending_pin_mac[18];
    char pending_pin_code[8];
} bt_audio_status_t;

/* Cópia thread-safe do estado atual (usado por web_server.c em /api/status). */
void bt_audio_get_status(bt_audio_status_t *out);

/* Chamar sempre que o volume local mudar por uma ação explícita do usuário
 * (web UI), nunca pelos ajustes contínuos do AGC — sincroniza o slider de
 * volume do celular via AVRCP (absolute volume), só se ele tiver pedido
 * notificação (ver ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT). Escala 0-VOLUME_STEPS. */
void bt_audio_notify_volume_changed(int volume_0_to_steps);

/* Remove o bond (link key) desse MAC no controlador Bluetooth e desconecta
 * agora mesmo se estiver conectado -- sem isso o celular reconecta sozinho
 * de novo em seguida (link key salva). Tambem revoga da lista de
 * autorizados (pairing_set_allowed). Usado pela API web pra "esquecer" um
 * dispositivo e liberar a conexao pra outro. */
void bt_audio_forget_device(const uint8_t mac[6]);

/* So desconecta -- nao mexe no bond nem na lista de autorizados, o
 * dispositivo pode reconectar depois normalmente. Usado pela API web pra
 * liberar a conexao pra outro dispositivo sem esquecer o atual. */
void bt_audio_disconnect_device(const uint8_t mac[6]);

/* Comando de controle de midia via AVRCP passthrough (o celular decide o
 * que fazer -- nos so mandamos o "botao"). cmd: "play", "pause",
 * "playpause" (alterna com base no ultimo estado conhecido), "stop",
 * "next", "previous". Retorna ESP_ERR_INVALID_ARG se cmd nao reconhecido. */
esp_err_t bt_audio_media_control(const char *cmd);

/* true = aparece na busca de dispositivos Bluetooth de quem ainda nao
 * pareou (padrao). false = fica invisivel pra pareamentos novos, mas
 * dispositivos ja pareados continuam conectando normalmente (isso e
 * "connectable", nao "discoverable", que continua sempre ligado). Aplica
 * na hora, sem precisar reiniciar. */
void bt_audio_set_discoverable(bool discoverable);
bool bt_audio_get_discoverable(void);

/* true = novos pareamentos exigem "Passkey Entry": um codigo de 6 digitos
 * gerado na hora e mostrado em /api/status (pending_pin_code), que a
 * pessoa deve digitar no celular. Dispositivos ja pareados nao sao
 * afetados. Aplica na hora, sem precisar reiniciar. */
void bt_audio_set_require_pin(bool require_pin);
bool bt_audio_get_require_pin(void);
