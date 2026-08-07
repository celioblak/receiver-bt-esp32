#pragma once

#include <stdint.h>

/* =========================================================================
 * Receiver Bluetooth DIY — configuração global do firmware
 * ESP32 Audio Kit V2.2 (módulo ESP32-A1S, codec ES8388)
 * ========================================================================= */

#define FW_VERSION "1.0.0"
#define FW_DEVICE_NAME_DEFAULT "Receiver-BT"

/* -------------------------------------------------------------------------
 * Pinagem (ESP32-A1S / ESP32 Audio Kit V2.2)
 * ------------------------------------------------------------------------- */

/* I2C — ES8388 (codec) e expansão futura (display OLED SSD1306) */
#define PIN_I2C_SDA        33
#define PIN_I2C_SCL        32

/* I2S — ligação entre ESP32 e ES8388 */
#define PIN_I2S_MCLK       0
#define PIN_I2S_BCLK       27
#define PIN_I2S_WS         25
#define PIN_I2S_DOUT       26   /* ESP32 -> codec */
#define PIN_I2S_DIN        35   /* codec -> ESP32 */

/* PA_ENABLE do amplificador onboard do kit — NUNCA ativar neste projeto.
 * O áudio sai pela saída de linha para o amplificador externo. */
#define PIN_PA_ENABLE_DO_NOT_USE 21

/* Controle do relé do amplificador externo (HIGH = liga, LOW = desliga).
 * Este GPIO também está ligado ao LED onboard do kit (ativo em nível baixo,
 * confirmado de forma independente pelo projeto squeezelite-esp32) — o LED
 * vai piscar/acender junto com o relé. É cosmético, não um conflito real. */
#define PIN_RELAY_CONTROL  22

/* -------------------------------------------------------------------------
 * NVS — namespace e chaves
 * ------------------------------------------------------------------------- */

#define NVS_NAMESPACE "recv_bt"

#define NVS_KEY_DEVICE_NAME     "dev_name"
#define NVS_KEY_VOLUME          "volume"
#define NVS_KEY_RELAY_TIMEOUT   "rly_timeout"
#define NVS_KEY_WIFI_SSID       "wifi_ssid"
#define NVS_KEY_WIFI_PASS       "wifi_pass"
#define NVS_KEY_MQTT_HOST       "mqtt_host"
#define NVS_KEY_MQTT_PORT       "mqtt_port"
#define NVS_KEY_MQTT_USER       "mqtt_user"
#define NVS_KEY_MQTT_PASS       "mqtt_pass"
#define NVS_KEY_PAIRED_MACS     "paired_macs"
#define NVS_KEY_DEVICE_HISTORY  "dev_history"
#define NVS_KEY_STAT_UPTIME_MIN "stat_uptime"
#define NVS_KEY_STAT_BT_CONNS   "stat_btconn"

/* -------------------------------------------------------------------------
 * Valores padrão
 * ------------------------------------------------------------------------- */

#define DEFAULT_VOLUME          70      /* 0-100 */
#define DEFAULT_RELAY_TIMEOUT_S 30      /* segundos sem PLAYING até desligar o ampli */
#define RELAY_SILENCE_DEBOUNCE_S 2      /* ignora pausas curtas entre faixas */

/* -------------------------------------------------------------------------
 * Logger (ring buffer em memória)
 * ------------------------------------------------------------------------- */

#define LOGGER_MAX_ENTRIES  100
#define LOGGER_MSG_MAX_LEN  128

/* -------------------------------------------------------------------------
 * Wi-Fi / mDNS
 * ------------------------------------------------------------------------- */

/* Subida do AP de configuração só quando não há credenciais salvas —
 * não é um fallback permanente para instabilidade de rede (ver
 * wifi_manager.c). Sem senha (rede aberta) para facilitar a config inicial. */
#define WIFI_AP_SSID_DEFAULT "ReceiverBT-Config"
#define WIFI_STA_CONNECT_TIMEOUT_MS 10000
#define MDNS_HOSTNAME "receiver-bt"
