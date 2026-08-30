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

/* Quantos dispositivos estao na lista de autorizados. Importa pra interface:
 * com a lista VAZIA qualquer um pode parear; com pelo menos um, so os da
 * lista conseguem -- abrir a janela de pareamento nao basta pra um aparelho
 * novo, ele seria rejeitado em pairing_is_allowed(). */
size_t pairing_get_allowed_count(void);

/* Copia os macs autorizados/bloqueados (thread-safe). Usado pra garantir que
 * a lista de dispositivos da interface mostre TODO mundo autorizado ou
 * bloqueado, mesmo os que não têm (ou perderam) entrada no histórico --
 * senão um mac bloqueado sem histórico fica invisível e sem jeito de
 * desbloquear pela interface. */
size_t pairing_get_allowed_list(uint8_t out[][6], size_t max_entries);
size_t pairing_get_blocked_list(uint8_t out[][6], size_t max_entries);

/* Remove uma entrada do histórico (não mexe nas listas de autorizados/
 * bloqueados). */
void pairing_remove_from_history(const uint8_t mac[6]);

/* Reset completo pra um mac: histórico + lista de autorizados + lista de
 * bloqueados, voltando ao estado neutro (nem autorizado nem bloqueado).
 * Usar em "esquecer"/"remover" -- ver comentário em pairing.c. */
void pairing_clear_device(const uint8_t mac[6]);

/* "Controle de dispositivo" (pedido explicito do usuario): ligado, o
 * PRIMEIRO dispositivo que completar um pareamento novo vira
 * automaticamente o unico autorizado (a lista deixa de estar vazia,
 * passando a restringir todo mundo depois dele -- ver pairing_is_allowed()).
 * Desligado (padrao, compativel com o comportamento de sempre): lista vazia
 * continua aceitando qualquer um, sem trava automatica. Persistido em NVS. */
bool pairing_get_lock_mode(void);
void pairing_set_lock_mode(bool enabled);

/* Converte "aa:bb:cc:dd:ee:ff" -> 6 bytes. Retorna false se o formato for inválido. */
bool pairing_parse_mac(const char *str, uint8_t mac[6]);
void pairing_format_mac(const uint8_t mac[6], char *out, size_t out_len);
