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
/* Tambem e o LED D4 da placa, ATIVO EM NIVEL BAIXO: com o rele desligado
 * (nivel 0, o estado de repouso) o LED fica ACESO, e ele apaga quando o
 * amplificador liga. Confirmado ao vivo em 2026-08-30 tocando musica e
 * olhando a placa -- a documentacao dizia o contrario. */
#define PIN_RELAY_CONTROL  22

/* LED VERMELHO onboard (D5), o que fica ao lado do jack de fone -- confirmado
 * ao vivo pelo Celio em 2026-08-30, piscando o pino e olhando a placa. As
 * referencias de terceiros divergiam: uma lista GPIO19 como LED D5, outra como
 * KEY3 (botao). Nesta placa e LED.
 *
 * O LED VERDE (D4) nao entra aqui: ele nao e controlavel por este firmware.
 * Ver a secao "Sinalizacao por LED" do README. */
#define PIN_STATUS_LED     19

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
#define NVS_KEY_BLOCKED_MACS    "blocked_macs"
#define NVS_KEY_PAIRING_LOCK    "pairing_lock"
#define NVS_KEY_DEVICE_HISTORY  "dev_history"
#define NVS_KEY_STAT_UPTIME_MIN "stat_uptime"
#define NVS_KEY_STAT_BT_CONNS   "stat_btconn"
#define NVS_KEY_BT_DISCOVERABLE "bt_discover"
#define NVS_KEY_MIC_ENABLED     "mic_enabled"
#define NVS_KEY_MIC_AUTO_GATE   "mic_auto_gate"
#define NVS_KEY_MIC_GAIN        "mic_gain"
#define NVS_KEY_MIC_GATE_LEVEL  "mic_gate_lvl"
#define NVS_KEY_MIC_INPUT       "mic_input"
#define NVS_KEY_RELAY_ACTIVE_LOW "relay_act_lo"
#define NVS_KEY_RELAY_OPEN_DRAIN "relay_od"

/* -------------------------------------------------------------------------
 * Valores padrão
 * ------------------------------------------------------------------------- */

#define DEFAULT_RELAY_TIMEOUT_S 30      /* segundos sem PLAYING até desligar o ampli */

/* Polaridade do módulo de relé. 0 = aciona em nível ALTO (o que este firmware
 * sempre assumiu); 1 = aciona em nível BAIXO.
 *
 * A maioria dos módulos de relé com optoacoplador é **low trigger**, e com eles
 * a lógica fica invertida: em repouso o GPIO está em nível 0, o relé fica
 * ACIONADO e o amplificador nunca desliga. Sintoma exato relatado pelo Célio ao
 * instalar (2026-08-30): "liguei o dispositivo, nada em reprodução, o relay já
 * fica ativo".
 *
 * Configurável em vez de fixo porque depende do módulo que estiver instalado, e
 * trocar módulo não deveria exigir recompilar. Ajustável em `/api/config`
 * (`relay_active_low`) e testável com `POST /api/amp`. */
#define DEFAULT_RELAY_ACTIVE_LOW 0

/* DRENO ABERTO no pino do relé. 0 = saída normal (push-pull); 1 = dreno aberto.
 *
 * Existe por um caso medido na instalação (2026-08-30): módulo de relé de 5V
 * com optoacoplador, ligado ao IO22. O relé ficava ACIONADO nos dois níveis e
 * só soltava quando o Célio REMOVIA o fio do IN.
 *
 * A causa é de nível: para DESLIGAR, esse módulo precisa ver o IN perto de 5V,
 * e o ESP32 entrega no máximo 3,3V -- sobra tensão suficiente sobre o
 * optoacoplador para mantê-lo conduzindo. Com o fio fora, o pull-up interno do
 * módulo leva o IN a 5V e ele solta.
 *
 * Dreno aberto reproduz exatamente isso: para acionar, o pino puxa para GND;
 * para desligar, fica em alta impedância -- eletricamente igual ao fio
 * removido, deixando o pull-up de 5V do módulo agir. Resolve sem transistor
 * nem conversor de nível.
 *
 * Só faz sentido junto com relay_active_low: em dreno aberto o pino não
 * consegue impor nível alto, então um módulo high trigger precisa de saída
 * normal (ou de um pull-up externo para 3,3V). */
#define DEFAULT_RELAY_OPEN_DRAIN 0
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

/* "Controle de dispositivo" -- ver pairing_get_lock_mode() em pairing.h.
 * 0 = mesmo comportamento de sempre (lista vazia aceita qualquer um). */
#define DEFAULT_PAIRING_LOCK    0

/* Entrada de microfone (ADC do ES8388, ver es8388.c/audio_codec.c) --
 * desligada por padrão. Habilitar aloca o canal I2S RX (full-duplex) e liga
 * o bloco ADC do codec, ambos com custo de RAM interna real (ver memória do
 * projeto sobre esse recurso ser escasso) -- só decidido uma vez, no boot,
 * por isso muda só depois de reiniciar. Recurso ainda sem hardware de teste
 * disponível (ver README, seção Notas) -- os valores de registrador do ADC
 * são a melhor referência disponível (datasheet ES8388 + driver ES-ADF do
 * mesmo family de placas), não validados em bring-up físico ainda. */
#define DEFAULT_MIC_ENABLED     0

/* Qual entrada analogica do codec o microfone usa -- indice de
 * es8388_mic_input_t (es8388.h). 1 = ES8388_IN_LIN2_SE, o par
 * LINPUT2/RINPUT2, que nesta placa e onde chega o JACK DE ENTRADA de
 * linha; os microfones embutidos estao em LINPUT1/RINPUT1.
 *
 * Era o modo diferencial LIN1-RIN1, que amplifica a DIFERENCA entre os
 * dois microfones embutidos -- eles captam quase o mesmo som, entao a voz
 * era cancelada na subtracao e sobrava o ruido de cada um. Ver o
 * comentario de historico em es8388.c. */
#define DEFAULT_MIC_INPUT       3

/* Deteccao automatica de voz no mic (noise gate) -- ligada por padrao.
 * Relato do usuario 2026-08-24: com o mic ligado, um chiado ficava
 * misturado na musica o tempo todo, mesmo sem ninguem falando -- o mixer
 * (write_with_mic_mix em audio_codec.c) somava QUALQUER coisa que o ADC
 * captasse, ruido de fundo do microfone incluso. Com o portao ligado, so
 * mistura o audio do mic quando o nivel captado passa de MIC_GATE_THRESHOLD
 * (ver audio_codec.c) -- silencio (nada de ruido de fundo) o resto do
 * tempo. Ao contrario de MIC_ENABLED, isso e so uma flag lida a cada bloco
 * de audio, entao aplica na hora, sem precisar reiniciar. */
#define DEFAULT_MIC_AUTO_GATE   1

/* Ganho digital do microfone na mixagem de karaoke, 0-100 (mapeado pra
 * 0..4x em audio_codec.c). 50 = 2x, MEDIDO 2026-08-29: o sinal do receptor
 * chega com pico de ~7600, entao 2x poe o pico em ~16000, metade da escala --
 * audivel e com folga pra nao saturar nos gritos. Ajustavel em tempo real
 * pela API/web, ja que o nivel util depende do microfone e da distancia. */
#define DEFAULT_MIC_GAIN        60

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
