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
#define NVS_KEY_BT_DISCOVERABLE "bt_discover"
#define NVS_KEY_BT_REQUIRE_PIN  "bt_req_pin"

/* -------------------------------------------------------------------------
 * Valores padrão
 * ------------------------------------------------------------------------- */

#define DEFAULT_RELAY_TIMEOUT_S 30      /* segundos sem PLAYING até desligar o ampli */
#define RELAY_SILENCE_DEBOUNCE_S 2      /* ignora pausas curtas entre faixas */

/* 0 = NÃO aparece na busca de aparelhos novos por padrão; para parear, abre-se
 * uma janela temporária ("Permitir pareamento", ver
 * bt_audio_enable_discoverable_temporary / DEFAULT_BT_PAIRING_WINDOW_S).
 * Aparelhos JÁ pareados continuam conectando normalmente -- isso é
 * "connectable", que segue sempre ligado. Era 1 (visível para sempre), o que
 * mantinha a varredura periódica de rádio do BT ativa o tempo todo disputando
 * CPU/rádio com a decodificação de áudio (ver memória do projeto sobre
 * engasgo), além de deixar o aparelho pareável por qualquer um indefinidamente. */
#define DEFAULT_BT_DISCOVERABLE 0

/* Duração padrão da janela de pareamento, em segundos. */
#define DEFAULT_BT_PAIRING_WINDOW_S 180
#define DEFAULT_BT_REQUIRE_PIN  0  /* 1 = exige passkey de 6 dígitos (mostrado na página Dispositivos) para novos pareamentos */

/* -------------------------------------------------------------------------
 * Volume fino (escala perceptual) — ver audio_codec.c
 * ------------------------------------------------------------------------- */

/* Volume exposto pela API/Web é 0-VOLUME_STEPS; audio_codec.c mapeia para
 * 0-100 no ES8388 com curva quadrática (x²), já que o ouvido é logarítmico
 * e a atenuação do DAC é linear em dB. */
#define VOLUME_STEPS       200
#define DEFAULT_VOLUME_USER 140  /* ~equivalente ao antigo default 70/100 */
#define NVS_KEY_VOLUME_USER "vol_user"

/* -------------------------------------------------------------------------
 * AGC (Automatic Gain Control) opcional — ver audio_agc.c
 * ------------------------------------------------------------------------- */

#define NVS_KEY_AGC_ENABLED "agc_enabled"
#define NVS_KEY_AGC_TARGET  "agc_target"
#define NVS_KEY_AGC_MODE    "agc_mode"

#define DEFAULT_AGC_ENABLED     0
#define DEFAULT_AGC_TARGET_DBFS (-18)  /* -30 a -6 */
#define DEFAULT_AGC_MODE        1      /* 0=suave 1=medio 2=agressivo */

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
