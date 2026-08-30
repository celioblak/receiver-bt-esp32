#include "pairing.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "logger.h"
#include "storage.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "pairing";

typedef struct {
    uint8_t count;
    uint8_t macs[PAIRING_MAX_ALLOWED][6];
} allowed_list_t;

typedef struct {
    uint8_t count;
    pairing_device_t entries[PAIRING_HISTORY_MAX]; /* [0] = mais recente */
} history_list_t;

static allowed_list_t s_allowed;
static allowed_list_t s_blocked; /* mesmo formato da lista de autorizados */
static history_list_t s_history;
static bool s_lock_mode;
static SemaphoreHandle_t s_mutex;

static bool mac_eq(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static int list_find(const allowed_list_t *list, const uint8_t mac[6])
{
    for (int i = 0; i < list->count; i++) {
        if (mac_eq(list->macs[i], mac)) {
            return i;
        }
    }
    return -1;
}

/* Remove pelo indice, se existir (>=0); no-op se nao. Retorna se removeu. */
static bool list_remove_at(allowed_list_t *list, int index)
{
    if (index < 0) {
        return false;
    }
    for (int i = index; i < list->count - 1; i++) {
        memcpy(list->macs[i], list->macs[i + 1], 6);
    }
    list->count--;
    return true;
}

/* Adiciona se ainda nao estiver e houver espaco; no-op caso contrario. */
static void list_add(allowed_list_t *list, const uint8_t mac[6])
{
    if (list_find(list, mac) < 0 && list->count < PAIRING_MAX_ALLOWED) {
        memcpy(list->macs[list->count], mac, 6);
        list->count++;
    }
}

static void save_allowed(void)
{
    storage_set_blob(NVS_KEY_PAIRED_MACS, &s_allowed, sizeof(s_allowed));
}

static void save_blocked(void)
{
    storage_set_blob(NVS_KEY_BLOCKED_MACS, &s_blocked, sizeof(s_blocked));
}

static void save_history(void)
{
    storage_set_blob(NVS_KEY_DEVICE_HISTORY, &s_history, sizeof(s_history));
}

void pairing_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    size_t len = sizeof(s_allowed);
    if (storage_get_blob(NVS_KEY_PAIRED_MACS, &s_allowed, &len) != ESP_OK || len != sizeof(s_allowed)) {
        memset(&s_allowed, 0, sizeof(s_allowed));
    }

    len = sizeof(s_history);
    if (storage_get_blob(NVS_KEY_DEVICE_HISTORY, &s_history, &len) != ESP_OK || len != sizeof(s_history)) {
        memset(&s_history, 0, sizeof(s_history));
    }

    len = sizeof(s_blocked);
    if (storage_get_blob(NVS_KEY_BLOCKED_MACS, &s_blocked, &len) != ESP_OK || len != sizeof(s_blocked)) {
        memset(&s_blocked, 0, sizeof(s_blocked));
    }

    int32_t lock_mode = DEFAULT_PAIRING_LOCK;
    storage_get_i32(NVS_KEY_PAIRING_LOCK, &lock_mode, DEFAULT_PAIRING_LOCK);
    s_lock_mode = (lock_mode != 0);

    logger_log(ESP_LOG_INFO, TAG,
               "Pareamento: %d dispositivo(s) autorizado(s), %d bloqueado(s), %d no historico, controle de dispositivo=%s",
               s_allowed.count, s_blocked.count, s_history.count, s_lock_mode ? "ligado" : "desligado");
}

bool pairing_get_lock_mode(void)
{
    return s_lock_mode;
}

void pairing_set_lock_mode(bool enabled)
{
    s_lock_mode = enabled;
    storage_set_i32(NVS_KEY_PAIRING_LOCK, enabled ? 1 : 0);
}

bool pairing_is_allowed(const uint8_t mac[6])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Bloqueio explicito tem prioridade sobre tudo -- inclusive sobre a
     * lista de autorizados vazia (que por padrao aceita qualquer um). Antes
     * "bloquear" so tentava REMOVER o mac da lista de autorizados -- se ele
     * nunca tinha sido adicionado ali (caso comum: lista vazia, aceita
     * todo mundo por padrao), a remocao nao encontrava nada e o clique em
     * "Bloquear" na interface nao tinha efeito nenhum (confirmado pelo
     * usuario testando pareamento real). Agora existe uma lista de
     * bloqueados de verdade, separada. */
    bool result;
    if (list_find(&s_blocked, mac) >= 0) {
        result = false;
    } else if (s_allowed.count > 0) {
        result = list_find(&s_allowed, mac) >= 0;
    } else {
        result = true;
    }
    xSemaphoreGive(s_mutex);
    return result;
}

void pairing_record_device(const uint8_t mac[6], const char *name)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int existing = -1;
    for (int i = 0; i < s_history.count; i++) {
        if (mac_eq(s_history.entries[i].mac, mac)) {
            existing = i;
            break;
        }
    }

    pairing_device_t entry;
    memcpy(entry.mac, mac, 6);
    strlcpy(entry.name, name ? name : "", sizeof(entry.name));
    entry.last_seen_ms = esp_timer_get_time() / 1000;

    if (existing >= 0) {
        /* remove da posição atual para reinserir no topo (mais recente primeiro) */
        for (int i = existing; i < s_history.count - 1; i++) {
            s_history.entries[i] = s_history.entries[i + 1];
        }
        s_history.count--;
    }

    /* insere no topo, empurrando os demais; descarta o mais antigo se cheio */
    int last = (s_history.count < PAIRING_HISTORY_MAX) ? s_history.count : PAIRING_HISTORY_MAX - 1;
    for (int i = last; i > 0; i--) {
        s_history.entries[i] = s_history.entries[i - 1];
    }
    s_history.entries[0] = entry;
    if (s_history.count < PAIRING_HISTORY_MAX) {
        s_history.count++;
    }

    save_history();
    xSemaphoreGive(s_mutex);

    logger_log(ESP_LOG_INFO, TAG, "Dispositivo registrado no historico: %s", name ? name : "?");
}

size_t pairing_get_history(pairing_device_t *out, size_t max_entries)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = s_history.count < max_entries ? s_history.count : max_entries;
    memcpy(out, s_history.entries, n * sizeof(pairing_device_t));
    xSemaphoreGive(s_mutex);
    return n;
}

size_t pairing_get_allowed_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = (size_t)s_allowed.count;
    xSemaphoreGive(s_mutex);
    return n;
}

static size_t copy_list(const allowed_list_t *list, uint8_t out[][6], size_t max_entries)
{
    size_t n = (size_t)list->count < max_entries ? (size_t)list->count : max_entries;
    for (size_t i = 0; i < n; i++) {
        memcpy(out[i], list->macs[i], 6);
    }
    return n;
}

size_t pairing_get_allowed_list(uint8_t out[][6], size_t max_entries)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = copy_list(&s_allowed, out, max_entries);
    xSemaphoreGive(s_mutex);
    return n;
}

/* Existe pra alimentar a lista de dispositivos da interface: um mac
 * autorizado/bloqueado sem entrada no historico (ex.: historico apagado
 * manualmente, ou -- ate esta correcao -- um bloqueio "fantasma" deixado
 * pelo bug do "Esquecer", ver pairing_clear_device()) ficava invisivel e
 * sem jeito de desfazer pela interface, porque /api/devices so listava a
 * partir do historico. */
size_t pairing_get_blocked_list(uint8_t out[][6], size_t max_entries)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = copy_list(&s_blocked, out, max_entries);
    xSemaphoreGive(s_mutex);
    return n;
}

void pairing_set_allowed(const uint8_t mac[6], bool allowed)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (allowed) {
        /* Autorizar sempre limpa um bloqueio anterior do mesmo mac -- os
         * dois estados sao mutuamente exclusivos, senao "autorizar" um
         * dispositivo bloqueado nao teria efeito nenhum. */
        list_remove_at(&s_blocked, list_find(&s_blocked, mac));
        list_add(&s_allowed, mac);
    } else {
        /* Bloquear remove de "autorizados" (se estava la) E registra o
         * bloqueio de verdade -- antes so fazia a remocao, que era um no-op
         * silencioso quando o mac nunca tinha sido adicionado a lista de
         * autorizados (o caso comum, ja que a lista vazia aceita qualquer
         * um por padrao). Ver pairing_is_allowed(). */
        list_remove_at(&s_allowed, list_find(&s_allowed, mac));
        list_add(&s_blocked, mac);
    }

    save_allowed();
    save_blocked();
    xSemaphoreGive(s_mutex);
}

void pairing_remove_from_history(const uint8_t mac[6])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int existing = -1;
    for (int i = 0; i < s_history.count; i++) {
        if (mac_eq(s_history.entries[i].mac, mac)) {
            existing = i;
            break;
        }
    }

    if (existing >= 0) {
        for (int i = existing; i < s_history.count - 1; i++) {
            s_history.entries[i] = s_history.entries[i + 1];
        }
        s_history.count--;
        save_history();
    }

    xSemaphoreGive(s_mutex);
}

void pairing_clear_device(const uint8_t mac[6])
{
    /* Reset completo pra esse mac: historico + autorizados + bloqueados --
     * pedido explicito do usuario apos confirmar um bug real causado pela
     * falta disso. bt_audio_forget_device() (action "forget") chamava so
     * pairing_set_allowed(mac, false) pra "limpar" o mac -- antes da lista
     * de bloqueados existir de verdade, isso era um no-op inofensivo. Depois
     * que "bloquear" passou a persistir um bloqueio de verdade (ver
     * pairing_set_allowed), essa mesma chamada em "esquecer" passou a
     * BLOQUEAR silenciosamente qualquer dispositivo esquecido -- pareamento
     * seguinte era rejeitado em ESP_BT_GAP_CFM_REQ_EVT sem nenhuma
     * explicacao visivel na interface (confirmado ao vivo: reconexao/
     * repareamento falhando, nada aparecia na lista de dispositivos, so
     * "pareou" na terceira tentativa). "Esquecer"/"Remover" devem voltar o
     * mac pro estado neutro (nem autorizado nem bloqueado), nao empurrar
     * ele pra bloqueado. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = list_remove_at(&s_allowed, list_find(&s_allowed, mac));
    changed |= list_remove_at(&s_blocked, list_find(&s_blocked, mac));
    if (changed) {
        save_allowed();
        save_blocked();
    }
    xSemaphoreGive(s_mutex);

    pairing_remove_from_history(mac);
}

bool pairing_parse_mac(const char *str, uint8_t mac[6])
{
    if (str == NULL) {
        return false;
    }
    unsigned int b[6];
    int n = sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (n != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

void pairing_format_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
