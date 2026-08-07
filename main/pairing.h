#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lista de dispositivos Bluetooth autorizados + histórico de conexões,
 * persistidos em NVS. Lista de autorizados vazia = aceita qualquer
 * dispositivo (comportamento padrão). Lista não vazia = só dispositivos
 * nela conseguem parear (rejeitado em ESP_BT_GAP_CFM_REQ_EVT, ver
 * bt_audio.c). */

#define PAIRING_MAX_ALLOWED  20
#define PAIRING_HISTORY_MAX  10

typedef struct {
    uint8_t mac[6];
    char name[32];
    int64_t last_seen_ms;
} pairing_device_t;

void pairing_init(void);

bool pairing_is_allowed(const uint8_t mac[6]);

/* Chamar quando um pareamento é concluído com sucesso (ESP_BT_GAP_AUTH_CMPL_EVT). */
void pairing_record_device(const uint8_t mac[6], const char *name);

/* Cópia thread-safe do histórico (mais recente primeiro). Retorna a
 * quantidade copiada. */
size_t pairing_get_history(pairing_device_t *out, size_t max_entries);

/* action: true = adiciona à lista de autorizados ("allow")
 *         false = remove da lista de autorizados ("block") */
void pairing_set_allowed(const uint8_t mac[6], bool allowed);

/* Remove uma entrada do histórico (não mexe na lista de autorizados). */
void pairing_remove_from_history(const uint8_t mac[6]);

/* Converte "aa:bb:cc:dd:ee:ff" -> 6 bytes. Retorna false se o formato for inválido. */
bool pairing_parse_mac(const char *str, uint8_t mac[6]);
void pairing_format_mac(const uint8_t mac[6], char *out, size_t out_len);
