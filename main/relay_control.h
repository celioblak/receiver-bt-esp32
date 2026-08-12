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
