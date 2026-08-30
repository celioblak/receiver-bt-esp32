#pragma once

#include <stdbool.h>

/* Fonte de audio ativa -- UMA de cada vez, sem concorrencia.
 *
 * Motivacao (2026-08-28, decisao do usuario): ate aqui Bluetooth e DLNA
 * conviviam ao mesmo tempo, disputando o mesmo codec/mutex de I2S. Praticamente
 * todos os defeitos investigados nesta fase do projeto nasceram dessa disputa
 * -- o pior deles um mutex que ficava orfao quando a task de I2S do BT era
 * destruida no meio de uma escrita (ver bt_i2s_task_stop em bt_audio.c),
 * deixando o audio mudo ate reiniciar.
 *
 * Novo modelo: um seletor explicito. So a fonte ativa tem tasks trabalhando;
 * a inativa fica bloqueada, sem consumir CPU e sem nunca tocar no codec.
 * Nao ha mais "prioridade do BT sobre o DLNA" -- quem manda e o seletor.
 *
 * Sempre inicia em BLUETOOTH depois de reiniciar (nao persiste em NVS, por
 * pedido explicito do usuario): o uso mais comum e o celular, e um estado
 * previsivel apos queda de energia vale mais que lembrar a ultima escolha.
 *
 * Troca pelo botao fisico KEY1 da placa (ver button_diag.c) ou pela API
 * (POST /api/source). */

typedef enum {
    AUDIO_SOURCE_BT = 0,
    AUDIO_SOURCE_DLNA = 1,
} audio_source_t;

/* Chamar uma vez no boot, antes de subir BT/DLNA. */
void audio_source_init(void);

audio_source_t audio_source_get(void);
bool audio_source_is_bt(void);
bool audio_source_is_dlna(void);

/* Nome curto pra log/API ("bluetooth" ou "dlna"). */
const char *audio_source_name(audio_source_t src);

/* Troca a fonte ativa. Silencia o codec na transicao e avisa os dois
 * subsistemas (ver audio_source_on_change_* em bt_audio.c/dlna_renderer.c).
 * Sem efeito se ja estiver na fonte pedida. */
void audio_source_set(audio_source_t src);

/* Alterna entre as duas -- usado pelo botao fisico. */
void audio_source_toggle(void);
