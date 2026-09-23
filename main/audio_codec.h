#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Caminho de áudio nativo do ESP-IDF: driver I2S (driver/i2s_std.h) + ES8388
 * (es8388.c). Sem ESP-ADF — ver README para o racional (colisão de nomes de
 * arquivo do ESP-ADF com o esquema de build do PlatformIO). */

esp_err_t audio_codec_init(void);

/* Volume 0-VOLUME_STEPS (escala perceptual, ver config.h). Mapeia
 * linearmente em dB (não quadrático) para o ES8388 (0-100) e persiste em
 * NVS (NVS_KEY_VOLUME_USER). */
esp_err_t audio_codec_set_volume(int volume);
int audio_codec_get_volume(void);

/* Como audio_codec_set_volume, mas NÃO persiste em NVS nem altera o valor
 * retornado por audio_codec_get_volume() — usado pelo AGC (audio_agc.c)
 * para modular o ganho de saída sem mexer no volume definido pelo usuário. */
esp_err_t audio_codec_apply_gain(int volume);

esp_err_t audio_codec_set_mute(bool mute);

/* Escreve PCM 16 bits estéreo já no sample rate configurado (ver
 * audio_codec_reconfigure_clock). Bloqueia até enviar tudo ou timeout. */
esp_err_t audio_codec_write(const uint8_t *data, size_t len, size_t *bytes_written);

/* esp_timer_get_time() da ultima chamada bem-sucedida de audio_codec_write()
 * com len>0 -- 0 se nunca escreveu nada ainda. Usado por um watchdog
 * (dlna_renderer.c) pra detectar "estado diz tocando mas nada sai de
 * verdade". */
int64_t audio_codec_last_write_us(void);

/* true enquanto audio_codec_reconfigure_clock() espera o mutex do I2S. As
 * tasks de I2S (bt_audio.c/dlna_renderer.c), que rodam em prioridade bem
 * mais alta, devem checar isso e ceder (dormir um pouco) antes de escrever
 * -- senao a reconfiguracao nunca consegue o mutex e o audio fica na taxa
 * errada/mudo (inanicao por prioridade, diagnosticada 2026-08-27). */
bool audio_codec_clock_change_pending(void);

/* A2DP pode informar sample rate diferente de faixa pra faixa (ex.: 44100 ou
 * 48000 Hz). Reconfigura o clock do I2S sem reinicializar o codec. */
esp_err_t audio_codec_reconfigure_clock(uint32_t sample_rate_hz);

/* Toca um tom curto (440Hz, ~0.6s) direto no codec, ligando o rele do
 * amplificador sozinho -- nao depende de Bluetooth, WiFi ou nenhum
 * protocolo de rede. Serve pra confirmar que o caminho fisico inteiro
 * (codec -> rele -> amplificador -> caixa) emite som de verdade,
 * independente de qualquer fonte de audio estar alcançando o dispositivo.
 * Disponivel sob demanda via /api/system/beep (ver web_server.c) -- NAO
 * chamado automaticamente no boot (ligar rele + I2S tao cedo, antes do
 * WiFi/BT estabilizarem, e suspeito de causar falha de boot standalone sem
 * USB/serial, ver main.c). Bloqueia pela duracao do tom. */
void audio_codec_play_test_tone(void);

/* Entrada de microfone (ADC do ES8388) -- desligada por padrão
 * (DEFAULT_MIC_ENABLED em config.h). Decidida uma única vez em
 * audio_codec_init() (lê NVS_KEY_MIC_ENABLED): se ligada, aloca o canal I2S
 * RX (full-duplex, mesmo MCLK/BCLK/WS do TX, DIN em PIN_I2S_DIN, buffer de
 * DMA bem mais raso que o TX -- ver i2s_init() em audio_codec.c) além do TX
 * de sempre -- se desligada, o comportamento é idêntico ao de antes dessa
 * funcionalidade existir (só TX). Não há como ligar/desligar em runtime sem
 * reiniciar (ver comentário em config.h). Quando ligada, `audio_codec_write`
 * mistura automaticamente o microfone em cima de qualquer áudio que esteja
 * tocando (Bluetooth ou DLNA) -- "modo karaokê", único uso do mic hoje. */
bool audio_codec_mic_is_enabled(void);

/* Lê PCM 16 bits estéreo capturado do microfone (mesmo sample rate do I2S).
 * Bloqueia até receber `len` bytes ou o timeout. Retorna
 * ESP_ERR_INVALID_STATE se o mic não estiver habilitado. Uso direto
 * opcional (ex. diagnóstico) -- a mixagem em audio_codec_write() lê o mic
 * por conta própria, não passa por aqui. */
esp_err_t audio_codec_mic_read(uint8_t *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);

/* Deteccao automatica de voz (noise gate) na mixagem do mic -- ligada por
 * padrão (DEFAULT_MIC_AUTO_GATE em config.h). Com o mic habilitado
 * (audio_codec_mic_is_enabled), write_with_mic_mix() só soma o áudio
 * captado quando o nível passa de um limiar (fala/canto de perto) --
 * silêncio o resto do tempo, em vez de misturar o ruído de fundo do
 * microfone o tempo todo. Ao contrário de mic_enabled, aplica em runtime
 * (sem reiniciar) e persiste em NVS_KEY_MIC_AUTO_GATE. */
void audio_codec_set_mic_auto_gate(bool enabled);
bool audio_codec_get_mic_auto_gate(void);

/* Ganho digital do microfone na mixagem (0-100 -> 0..4x) e limiar do portao
 * automatico. Ambos aplicam NA HORA e persistem em NVS -- a ideia e cantar e
 * regular ao vivo pela pagina/API, ja que o nivel util depende do microfone
 * e da distancia da boca. audio_codec_get_mic_peak() devolve o pico do
 * ultimo bloco captado (0-32767), util como "medidor" pra saber se o
 * microfone esta captando e se o limiar do portao esta bem escolhido. */
void audio_codec_set_mic_gain(int gain_0_to_100);

/* Realce de agudos do microfone, 0-100 (0 desliga). Compensa uma fonte que
 * entrega abafada -- ver DEFAULT_MIC_TREBLE em config.h. */
void audio_codec_set_mic_treble(int nivel_0_to_100);
int audio_codec_get_mic_treble(void);

/* Limitador de picos do microfone, 0-100 (0 desliga). Permite subir o volume
 * medio -- necessario para a voz competir com a musica de fundo -- sem que os
 * picos batam no teto. Ver DEFAULT_MIC_LIMITER em config.h. */
void audio_codec_set_mic_limiter(int nivel_0_to_100);
int audio_codec_get_mic_limiter(void);
int audio_codec_get_mic_gain(void);
void audio_codec_set_mic_gate_threshold(int threshold);
int audio_codec_get_mic_gate_threshold(void);
int audio_codec_get_mic_peak(void);

/* Em que estado o ADC do microfone subiu neste boot: "saudavel", "travado",
 * "chiando" ou "medindo". O conversor desta placa sobe aleatoriamente em um
 * de tres estados e nada em runtime corrige -- so reiniciar. Fora de
 * "saudavel" o audio do microfone NAO e misturado na saida, pra nao despejar
 * chiado em cima da musica. Exposto em /api/status pra interface poder
 * avisar e oferecer o reinicio. */
const char *audio_codec_get_mic_adc_estado(void);

/* Captura amostras CRUAS do microfone (canal esquerdo, antes de filtro,
 * portao, ganho e mixagem) -- diagnostico. Ver GET /api/mic/raw. */
size_t audio_codec_mic_capture_raw(int16_t *dest, size_t max_amostras);

/* Escolhe qual canal do conversor a captura crua devolve (false = esquerdo,
 * que e o usado pelo audio). Diagnostico -- ver GET /api/mic/raw?ch=r. */
void audio_codec_mic_raw_set_canal(bool direito);

/* Ultimo bloco JA PROCESSADO (o que vai para o alto-falante), para conferir se
 * um ajuste teve efeito de verdade. Ver GET /api/mic/raw?stage=out. */
size_t audio_codec_mic_capture_saida(int16_t *dest, size_t max_amostras);

/* Quantas amostras saturaram no ultimo segundo de saida. Zero = folga.
 * Qualquer valor acima disso significa que o ganho esta alto demais para a
 * fonte -- e saturacao soa ABAFADA, nao alta. */
int audio_codec_mic_get_clip(void);

/* Quantas amostras o limitador segurou no ultimo segundo. Zero = ele nao esta
 * atuando; valores altos = esta trabalhando muito (ou o ganho esta demais). */
int audio_codec_mic_get_limit_hits(void);

/* Liga/desliga o microfone em RUNTIME, sem reiniciar o aparelho, e persiste em
 * NVS. Ligar cria o canal I2S RX naquele instante e mede em que estado o ADC
 * subiu; desligar o remove. Como o RX e a unica coisa que sobe instavel nesta
 * placa, manter o microfone desligado deixa Bluetooth e DLNA sempre limpos --
 * e, se o microfone subir ruim, desligar e ligar de novo tenta outra vez sem
 * derrubar a musica. */
esp_err_t audio_codec_mic_set_enabled(bool on);

/* Entrada analogica do codec usada pelo microfone -- indice de
 * es8388_mic_input_t (es8388.h). Aplica na hora (e so uma escrita de I2C,
 * nao mexe no I2S) e persiste em NVS.
 *
 * Existe porque esta placa tem os microfones embutidos e o jack de entrada em
 * pares DIFERENTES do codec, e por muito tempo este firmware leu o par
 * errado. Deixar a escolha exposta evita que a proxima duvida do tipo custe
 * um ciclo de compilar/gravar por palpite. */
esp_err_t audio_codec_mic_set_input(int modo);
int audio_codec_mic_get_input(void);
const char *audio_codec_mic_get_input_name(void);


/* Derruba o MCLK por 500ms e reconstroi I2S e codec do zero -- o unico jeito
 * conhecido de tirar o conversor do estado em que ele sobe gerando ruido, sem
 * reiniciar o aparelho. Ver o comentario na implementacao. */
esp_err_t audio_codec_mic_hard_reset(void);

