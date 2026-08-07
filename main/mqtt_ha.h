#pragma once

/* Integração opcional com Home Assistant via MQTT Discovery. Sem broker
 * configurado (NVS_KEY_MQTT_HOST vazio), não faz nada — não trava o
 * sistema nem exige rede/MQTT para o resto do firmware funcionar. */

void mqtt_ha_init(void);

/* Publica o estado atual (device/faixa/volume/amplificador/ip) no tópico
 * homeassistant/sensor/receiver_bt/state. Sem efeito se não conectado. */
void mqtt_ha_publish_state(void);
