#pragma once

#include <stdbool.h>

/* Controle do relé do amplificador externo (GPIO22). Liga imediatamente
 * quando o A2DP está tocando; desliga após um timeout sem tocar (padrão
 * DEFAULT_RELAY_TIMEOUT_S, configurável via NVS_KEY_RELAY_TIMEOUT). O
 * timeout, por ser maior que pausas normais entre faixas, já funciona como
 * debounce — não é preciso um mecanismo de debounce separado. */

void relay_control_init(void);

/* Chamar a cada mudança de estado do A2DP (tocando/pausado/desconectado). */
void relay_control_notify_playing(bool playing);

/* Desliga AGORA, sem esperar o timeout -- diferente de
 * relay_control_notify_playing(false), que só arma o timer (debounce pra
 * pausas curtas entre faixas). Usar só em ações explícitas e deliberadas
 * do usuário (ex.: botão "Desconectar"/"Esquecer" na interface web), onde
 * esperar o timeout não faz sentido. */
void relay_control_force_off(void);

bool relay_control_is_on(void);

/* Liga AGORA e cancela o timer de desligamento -- para testar a instalação do
 * relé sem depender de haver áudio tocando. Ver POST /api/amp. */
void relay_control_force_on(void);

/* Polaridade do módulo de relé: true = aciona em nível BAIXO.
 *
 * A maioria dos módulos com optoacoplador é low trigger, e com eles a lógica
 * fica invertida -- em repouso o GPIO está em 0, o relé fica acionado e o
 * amplificador nunca desliga. Persistido em NVS; ajustável por
 * `/api/config` (`relay_active_low`) sem recompilar. */
void relay_control_set_active_low(bool active_low);
bool relay_control_get_active_low(void);

/* Pino do relé em DRENO ABERTO: para acionar puxa para GND, para desligar fica
 * em alta impedância -- eletricamente igual a desconectar o fio.
 *
 * É o que faz funcionar um módulo de 5V com optoacoplador: ele precisa ver o IN
 * perto de 5V para desligar, e o ESP32 só entrega 3,3V. Ver
 * DEFAULT_RELAY_OPEN_DRAIN em config.h. */
void relay_control_set_open_drain(bool open_drain);
bool relay_control_get_open_drain(void);
