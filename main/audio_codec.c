#include "audio_codec.h"

#include "driver/gpio.h"

#include <inttypes.h>
#include <math.h>

#include <string.h>

#include "bt_audio.h"
#include "config.h"
#include "es8388.h"
#include "logger.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "relay_control.h"
#include "storage.h"

static const char *TAG = "audio_codec";

static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL; /* NULL quando o mic está desligado (ver audio_codec_mic_is_enabled) */
/* "O usuario quer o microfone" (lido do NVS no boot) -- separado de
 * s_rx_handle, que e "o canal RX ja existe". Os dois divergem de proposito
 * durante os primeiros segundos: com a inicializacao postergada, o canal so
 * nasce ~12s depois do boot, e ate la s_rx_handle e NULL mesmo com o mic
 * habilitado. Antes desta flag existir, a checagem era feita so por
 * s_rx_handle, entao postergar o canal desligava o microfone de vez (a task
 * nem chegava a ser criada, e a promocao nunca acontecia). */
static bool s_mic_wanted = false;
/* esp_timer_get_time() da ultima escrita de audio -- usado pra saber se ha
 * musica tocando. Declarado aqui no topo porque audio_codec_mic_set_enabled()
 * o consulta antes de decidir um reinicio automatico. */
static volatile int64_t s_last_write_us = 0;
/* true enquanto a recuperacao automatica do ADC esta rodando -- libera o
 * rate limit de audio_codec_mic_set_enabled() para o fluxo interno. */
static bool s_recuperacao_em_curso = false;
/* Em que estado o ADC subiu, decidido UMA vez logo apos a promocao do canal
 * (ver mic_live_task). O conversor desta placa sobe de forma aleatoria em um
 * de tres estados e nada em runtime corrige -- so reiniciar. Enquanto ele nao
 * estiver saudavel, o audio do microfone NAO e misturado na saida: melhor
 * ficar sem microfone ate reiniciar do que despejar chiado em cima da musica
 * (relato do Celio 2026-08-29, depois de o aparelho subir chiando 2x e travado
 * 2x em 5 reinicios seguidos). 0=ainda medindo, 1=saudavel, 2=travado,
 * 3=chiando. */
#define MIC_ADC_MEDINDO  0
#define MIC_ADC_OK       1
#define MIC_ADC_TRAVADO  2
#define MIC_ADC_CHIANDO  3
static volatile int s_mic_adc_estado = MIC_ADC_MEDINDO;
/* Karaokê: buffers de mixagem do mic sobre a música, um pra guardar a cópia
 * mutável do trecho de música (mixado em-lugar) e outro pro PCM cru lido do
 * mic -- ambos em PSRAM (grandes, e nada aqui é hot-path de latência crítica
 * a ponto de precisar de RAM interna). NULL quando o mic está desligado. */
/* Era 2048 (512 frames, ~11,6ms). Este bloco e o passo do laco da MIXAGEM,
 * ou seja, o caminho que vale quando ha musica tocando -- que e o caso real
 * do karaoke. Entra inteiro na latencia percebida, pelo mesmo motivo do
 * MIC_LIVE_CHUNK_BYTES abaixo, entao acompanha a mesma reducao. */
#define MIC_MIX_CHUNK_BYTES 1024
/* Bloco lido do microfone de cada vez (passagem direta, medicao do piso e
 * captura crua) -- definido aqui no topo porque varias funcoes o usam. */
/* Era 1024 (256 frames, ~5,8ms). Cada bloco lido e um passo do laco da
 * passagem direta, entao ele entra inteiro na latencia percebida. Com 512 o
 * passo cai para ~2,9ms. */
#define MIC_LIVE_CHUNK_BYTES 512
static uint8_t *s_mic_mix_music_buf = NULL;
static uint8_t *s_mic_mix_read_buf = NULL;
static int s_current_volume = DEFAULT_VOLUME_USER;
/* Nivel minimo (amplitude de pico, escala int16) pra considerar que ha
 * alguem falando/cantando de verdade no mic -- abaixo disso e ruido de
 * fundo do microfone onboard (ver audio_codec_set_mic_auto_gate). Ponto de
 * partida conservador (bem acima do que se espera de ruido de fundo, bem
 * abaixo de fala/canto normal de perto) -- ajustavel se precisar mais ou
 * menos sensibilidade quando houver hardware pra medir de verdade. */
/* CALIBRADO EM HARDWARE (2026-08-29), com a fonte do sinal comprovada (o
 * receptor sem fio do usuario, nao o microfone da placa -- ver es8388.c):
 *
 *   piso em silencio ........  97, picos ate 129
 *   voz pelo receptor ....... 2393 de media, picos de 16092
 *
 * 400 fica com folga dos dois lados: mudo parado, abre na voz.
 *
 * DEPENDE DO GANHO: se mudar o PGA ou o ALC, recalibrar este numero junto --
 * o piso acompanha. Ja aconteceu de mexer num sem o outro e o microfone
 * simplesmente emudecer (o limiar ficou acima da voz). */
/* RECALIBRADO (2026-08-30) junto com a troca da entrada. Era 2600, e o
 * aparelho estava gravado com 3400 -- numeros que faziam sentido quando o
 * modo diferencial dos mics embutidos deixava o piso em ~800. Com a entrada
 * correta o piso caiu para mediana ~30, e um portao em 3400 exigiria um grito
 * para abrir: o microfone pareceria morto sem estar.
 *
 * 300 e ~10x o piso medido -- ponto de partida seguro, nao um valor validado
 * cantando. Afinar com o medidor "Nivel captado agora" da pagina de
 * configuracoes: tem de ficar abaixo do nivel em silencio e bem abaixo do
 * nivel cantando. */
#define MIC_GATE_THRESHOLD 300
/* O contador de HOLD em blocos saiu junto com o portao binario: quem segura
 * o fim das palavras agora e o release da rampa (MIC_GATE_RELEASE_PASSO), que
 * desce o ganho progressivamente em vez de manter tudo aberto e cortar seco. */

/* EXPANSOR COM RAMPA -- substitui o corte binario do portao.
 *
 * MEDIDO 2026-08-30, com o Celio cantando e depois em silencio:
 *
 *   receptor DESLIGADO ............. mediana    5, pico   27
 *   receptor LIGADO, em silencio ... mediana 1408, pico 4700
 *   receptor LIGADO, cantando ...... mediana 4972, pico 16165
 *
 * Ou seja: o chiado NAO e da placa (mediana 5 e o fundo do conversor). E o
 * ruido de fundo do proprio receptor sem fio, e nos so o reproduzimos fiel.
 * Nao ha o que corrigir na captacao -- o que da pra fazer e nao deixar esse
 * piso passar quando ninguem esta cantando.
 *
 * O portao binario anterior errava justamente nas bordas, que foi o que o
 * Celio descreveu ("chiado no final ou inicio"):
 *  - ABERTURA de uma vez: o bloco inteiro entra num degrau, piso junto.
 *  - HOLD de 60 blocos (~700ms a 44,1kHz): depois de cada frase o piso do
 *    receptor passava inteiro por sete decimos de segundo.
 *
 * Agora o ganho e continuo e caminha AMOSTRA A AMOSTRA: sobe rapido quando a
 * voz chega (nao corta o ataque da palavra) e desce devagar quando ela para
 * (nao corta o fim), terminando em -30dB, onde o piso de 1400 vira ~44 --
 * abaixo do fundo audivel. Sem degrau, nao ha estalo. */
#define MIC_GATE_ESCALA      65536  /* 1.0 em ponto fixo de 16 bits */
/* -30dB. Nao zero: silenciar por completo faz o ambiente "sumir" de forma
 * artificial entre as frases, e o retorno fica perceptivel. */
#define MIC_GATE_GANHO_MIN   (MIC_GATE_ESCALA / 32)
/* ~3ms de subida (132 amostras a 44,1kHz) -- rapido o bastante para nao
 * comer o comeco da palavra, lento o bastante para nao estalar. */
/* ~1,5ms de subida. Era 3ms; com o envelope respondendo na hora, encurtar
 * aqui tira o resto do atraso sem trazer estalo de volta -- 1,5ms continua
 * muito acima do degrau instantaneo que causa clique. */
#define MIC_GATE_ATTACK_PASSO 960
/* ~250ms de descida. E o que substitui o hold: em vez de segurar aberto e
 * depois cortar seco, o ganho escorrega ate o piso. */
#define MIC_GATE_RELEASE_PASSO 6

/* FILTRO PASSA-BAIXA do audio do microfone (2026-08-29, pedido do Celio:
 * "ainda ha um chiado, acredito que precisa ser filtrada").
 *
 * O chiado desta montagem e ruido de banda larga, concentrado nas altas --
 * a voz cantada vive abaixo de ~4kHz. Um passa-baixa de 1 polo em ~5kHz
 * derruba boa parte do chiado e quase nao toca na voz.
 *
 * IIR de primeira ordem em ponto fixo: y += (x - y) * K / 256.
 * K = 256 * (2*pi*fc/fs) / (1 + 2*pi*fc/fs), com fc=5kHz e fs=44,1kHz -> ~106.
 * Barato (uma multiplicacao e um shift por amostra) e roda no mesmo laco que
 * ja converte pra mono. */
#define MIC_LPF_K 106

/* SUPRESSOR DE IMPULSO (filtro de mediana de 3 pontos), 2026-08-29.
 *
 * A captura crua do ADC (/api/mic/raw) revelou o que o usuario ouvia como
 * "chiado": NAO e ruido continuo nem rajada. Sao IMPULSOS DE UMA UNICA
 * AMOSTRA, muito frequentes -- sobre um fundo de mediana 32, medimos 306
 * eventos acima de 3x em 186ms, ou seja ~1645 por segundo, com duracao
 * mediana de 1 amostra (0,02ms). Milhares de cliques por segundo soam como
 * chiado continuo.
 *
 * Por isso nada funcionava: portao nao corta impulso (curto e alto), o
 * passa-baixa nao remove (impulso e banda larga), ganho nao muda a proporcao
 * e buffer maior nao ajuda (nao e perda de bloco, e amostra corrompida).
 *
 * A mediana de 3 pontos e o tratamento classico: se uma amostra destoa das
 * duas vizinhas, ela e substituida pela do meio do trio. Impulso isolado
 * desaparece; sinal de audio real (que varia suavemente entre amostras
 * consecutivas) passa intacto. Custa duas comparacoes por amostra. */
static inline int16_t mediana3(int16_t a, int16_t b, int16_t c)
{
    if (a > b) { int16_t t = a; a = b; b = t; }
    if (b > c) { int16_t t = b; b = c; c = t; }
    if (a > b) { b = a; }
    return b;
}
/* Quantas vezes a task tenta reinicializar o bloco do ADC quando ele sobe
 * travado (ver mic_live_task). O ADC desta placa sobe mudo em ~4 de 5 boots,
 * entao algumas tentativas resolvem; um teto evita laco eterno se a causa for
 * outra. */
#define MIC_MAX_TENTATIVAS_RECUPERACAO 3
/* Quantas vezes o firmware corta o MCLK e refaz o codec quando o conversor
 * sobe ruidoso. Cada passada leva ~1,5s e vale por uma nova tentativa
 * independente; com ~83% de acerto por tentativa, 4 passadas deixam a chance
 * de sobrar ruim abaixo de 1 em 1000. Tudo acontece nos primeiros segundos do
 * boot, antes de alguem usar o microfone. */
#define MIC_MAX_HARD_RESETS 4
/* NAO existe mais reinicio automatico quando o ADC sobe ruim. Ele chegou a
 * ser escrito (2026-08-30) e foi descartado no mesmo dia, junto com a
 * correcao da ordem de enable do I2S (ver i2s_init): era um remendo em cima
 * de uma causa raiz que agora esta corrigida, e enquanto ele existisse era
 * impossivel MEDIR a taxa real de boot -- o firmware escondia as falhas
 * reiniciando. Se o ADC voltar a subir ruim, isso tem que aparecer. */
/* Nao existe mais limiar MINIMO: um ADC saudavel com a entrada em silencio
 * absoluto pode dar mediana 0 legitimamente. Quem distingue "sem sinal" de
 * "canal morto" e o pico bruto (ver o detector). */
/* Limiar da MEDIANA do modulo das amostras, com a entrada em silencio.
 * Medido em 2026-08-30, ja com a entrada correta selecionada:
 *
 *   ADC saudavel .......... mediana    6 a  30   (pico 86-124)
 *   ADC subiu ruidoso ..... mediana ~1350        (pico ~7000)
 *
 * Sao mais de 40x de separacao, entao 200 fica bem no meio e nao encosta em
 * nenhum dos dois. O valor anterior (4000, sobre o PICO) era herdado de quando
 * a entrada errada fazia o piso legitimo passar de 800 -- com a entrada certa
 * o ruido de fundo despencou e o limiar velho aceitaria ate o ADC ruidoso. */
#define MIC_PISO_MAX_SAUDAVEL 200

/* Quanto o detector joga fora antes de medir, e quanto ele mede -- em
 * AMOSTRAS, para nao depender do tamanho do bloco. A 44,1kHz estereo sao
 * ~68ms de descarte e ~180ms de janela; o descarte cobre o transiente de
 * acordar o codec e a janela e longa o bastante para a mediana ser estavel. */
#define MIC_DETECT_DESCARTE 6000
#define MIC_DETECT_JANELA   16000
static volatile bool s_mic_auto_gate = DEFAULT_MIC_AUTO_GATE;
/* Ajustaveis em tempo real (ver audio_codec_set_mic_gain/threshold) -- a
 * ideia e o usuario cantar e regular na hora, sem recompilar. */
static volatile int32_t s_mic_digital_gain_x100 = DEFAULT_MIC_GAIN;      /* 100 = 1.0x */
static volatile int32_t s_mic_gate_threshold = MIC_GATE_THRESHOLD;
/* Ganho corrente do expansor, em MIC_GATE_ESCALA. Preservado entre blocos --
 * e justamente a continuidade dele que elimina o estalo de borda. */
static int32_t s_mic_gate_gain = MIC_GATE_GANHO_MIN;
/* Envelope do sinal, preservado entre blocos. Existe para que a deteccao
 * responda DENTRO do bloco: antes o ganho era decidido pelo pico do bloco
 * inteiro, entao a voz so "ligava" no bloco seguinte ao que a continha -- era
 * parte do atraso que o Celio ouviu no comeco das palavras. */
static int32_t s_mic_envelope = 0;
static volatile int16_t s_mic_peak = 0;
/* Minimo e maximo do ultimo bloco, ao lado do pico. Existem porque o pico
 * sozinho enganava: se o ADC entregar um nivel CONTINUO, o pico aparece alto
 * e mesmo assim nao sai som. min ~= max denuncia isso na hora. */
static volatile int16_t s_mic_min = 0;
static volatile int16_t s_mic_max = 0;
void audio_codec_set_mic_gain(int gain_0_to_100)
{
    if (gain_0_to_100 < 0) {
        gain_0_to_100 = 0;
    } else if (gain_0_to_100 > 100) {
        gain_0_to_100 = 100;
    }
    /* 0-100 -> 0..400 (ate 4x). Acima disso o ruido do mic onboard domina. */
    s_mic_digital_gain_x100 = gain_0_to_100 * 4;
    storage_set_i32(NVS_KEY_MIC_GAIN, gain_0_to_100);
}

int audio_codec_get_mic_gain(void)
{
    return (int)(s_mic_digital_gain_x100 / 4);
}

void audio_codec_set_mic_gate_threshold(int threshold)
{
    if (threshold < 0) {
        threshold = 0;
    } else if (threshold > 8000) {
        threshold = 8000;
    }
    s_mic_gate_threshold = threshold;
    storage_set_i32(NVS_KEY_MIC_GATE_LEVEL, threshold);
}

int audio_codec_get_mic_gate_threshold(void)
{
    return (int)s_mic_gate_threshold;
}

/* Captura amostras CRUAS do ADC -- antes do filtro, do portao, do ganho e da
 * mixagem. Existe porque a investigacao inteira ate 2026-08-29 foi feita
 * olhando so o "pico" de cada bloco, um numero resumido que esconde a forma do
 * sinal. Com as amostras na mao da pra responder de onde vem o chiado
 * (conversor? entrada analogica? processamento?) em vez de adivinhar.
 *
 * Escreve ate max_amostras int16 em `dest` e devolve quantas escreveu. */
size_t audio_codec_mic_capture_raw(int16_t *dest, size_t max_amostras)
{
    if (dest == NULL || max_amostras == 0 || s_rx_handle == NULL) {
        return 0;
    }
    int16_t *tmp = heap_caps_malloc(MIC_LIVE_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (tmp == NULL) {
        return 0;
    }
    size_t escritas = 0;
    while (escritas < max_amostras) {
        size_t lidos = 0;
        if (i2s_channel_read(s_rx_handle, tmp, MIC_LIVE_CHUNK_BYTES, &lidos,
                             pdMS_TO_TICKS(200)) != ESP_OK || lidos < 4) {
            break;
        }
        /* So o canal esquerdo (o unico com sinal nesta placa, ver es8388.c). */
        for (size_t i = 0; i + 1 < lidos / sizeof(int16_t) && escritas < max_amostras; i += 2) {
            dest[escritas++] = tmp[i];
        }
    }
    heap_caps_free(tmp);
    return escritas;
}

const char *audio_codec_get_mic_adc_estado(void)
{
    switch (s_mic_adc_estado) {
    case MIC_ADC_OK:      return "saudavel";
    case MIC_ADC_TRAVADO: return "travado";
    case MIC_ADC_CHIANDO: return "chiando";
    default:              return "medindo";
    }
}

int audio_codec_get_mic_peak(void)
{
    return (int)s_mic_peak;
}
/* Nada protegia o canal I2S contra chamadas concorrentes de tasks
 * diferentes -- bt_audio (task dedicada), a task de streaming de rede
 * (escreve direto do seu loop) e o beep sob demanda (/api/system/beep, roda
 * na task do httpd) podiam todos cair em audio_codec_write()/
 * reconfigure_clock() ao mesmo tempo. reconfigure_clock() em especial
 * desabilita o canal no meio do caminho -- se isso acontecer enquanto outra
 * task esta bloqueada dentro de um i2s_channel_write() em andamento, e um
 * cenario real de trava. Confirmado na pratica: beep disparado durante
 * playback de rede travou o dispositivo (precisou reset). */
static SemaphoreHandle_t s_i2s_mutex = NULL;
/* Bloco lido do microfone de cada vez na passagem direta e na medicao do piso
 * (ver mic_live_task e audio_codec_mic_set_enabled). Definido aqui no topo
 * porque as duas o usam. */
/* Definida mais abaixo; audio_codec_mic_set_enabled() a usa pra recriar o par
 * de canais ao ligar/desligar o microfone. */
static esp_err_t i2s_init(uint32_t sample_rate_hz, bool mic_enabled);
/* Espelha a taxa configurada em i2s_init(44100). Usado por
 * audio_codec_reconfigure_clock() pra pular o disable/reenable do canal
 * quando a taxa nao muda -- toda troca de faixa via streaming de rede chama
 * essa funcao, mesmo quando o formato e identico ao da faixa anterior
 * (comum, ja que a biblioteca inteira do usuario e 44.1kHz/24-bit), e cada
 * disable/reenable desnecessario do canal I2S produz um "click" audivel
 * na troca -- o proprio engasgo residual reportado depois de ja corrigido
 * o vazamento de audio entre faixas. */
static uint32_t s_current_sample_rate_hz = 44100;

/* Definida bem abaixo (perto do resto do codigo de microfone), mas criada
 * ainda em audio_codec_init() -- por isso o prototipo aqui. */
/* Liga/desliga o microfone SEM reiniciar o aparelho.
 *
 * Por que existe (pedido do Celio, 2026-08-29): o canal I2S RX so nasce quando
 * o microfone esta ligado, e e justamente ele que sobe travado ou chiando de
 * forma aleatoria. Com o microfone desligado o aparelho NUNCA tem esse
 * problema -- Bluetooth e DLNA sempre sobem limpos. Deixar o microfone
 * desligado por padrao e liga-lo sob demanda troca "instabilidade o tempo
 * todo" por "instabilidade so quando vou cantar".
 *
 * LIMITE IMPORTANTE, medido 2026-08-29: ligar o microfone em runtime quase
 * sempre resulta em ADC TRAVADO -- 8 tentativas seguidas de desliga/liga,
 * todas travadas. O canal RX so sobe saudavel quando criado UMA vez logo apos
 * o boot (ver mic_live_task); depois disso o periferico nao volta a um estado
 * bom sem reset de hardware. Isso e coerente com o resto do que foi medido:
 * destruir e recriar canais nunca recuperou um ADC travado, e refazer a
 * promocao piorou de 5/6 para 1/6.
 *
 * Ou seja: DESLIGAR em runtime funciona bem (silencia na hora, sem reiniciar);
 * LIGAR em runtime provavelmente vai dar travado, e ai so reiniciando com
 * NVS_KEY_MIC_ENABLED=1 pra o canal nascer junto do boot. O valor de ligar
 * aqui e sobretudo persistir a escolha e dar o retorno imediato do estado.
 *
 * O canal TX e recriado nos dois sentidos porque em full-duplex o par tem de
 * nascer numa unica chamada de i2s_new_channel() (ver i2s_init). */
esp_err_t audio_codec_mic_set_enabled(bool on)
{
    if (s_i2s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* PROTECAO CONTRA ACIONAMENTO REPETIDO (2026-08-30).
     *
     * Destruir e recriar os canais I2S em sequencia rapida TRAVA o firmware:
     * 8 ciclos de desliga/liga seguidos derrubaram o servidor web (o aparelho
     * ainda respondia a ping, mas nao a HTTP) e exigiram ciclo de energia. O
     * driver precisa de tempo pra soltar os recursos.
     *
     * Como o botao da interface chama isto, dois cliques seguidos bastariam
     * pra travar o dispositivo do usuario. Intervalo minimo de 3s entre
     * acionamentos -- pedidos mais rapidos sao recusados em vez de enfileirados. */
    static int64_t ultimo_us;
    int64_t agora = esp_timer_get_time();
    /* O rate limit existe contra CLIQUE REPETIDO do usuario. A recuperacao
     * automatica (hard reset) e fluxo interno e precisa de varias passadas
     * seguidas nos primeiros segundos do boot -- ela sinaliza aqui. */
    if (!s_recuperacao_em_curso && ultimo_us != 0 && (agora - ultimo_us) < 3000000) {
        logger_log(ESP_LOG_WARN, TAG,
                   "mic: liga/desliga pedido cedo demais (%lldms) -- ignorado pra nao travar o I2S",
                   (long long)((agora - ultimo_us) / 1000));
        return ESP_ERR_INVALID_STATE;
    }
    ultimo_us = agora;
    if (on == (s_rx_handle != NULL)) {
        s_mic_wanted = on;
        return ESP_OK; /* ja esta no estado pedido */
    }
    if (xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_tx_handle != NULL) {
        i2s_channel_disable(s_tx_handle);
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
    if (s_rx_handle != NULL) {
        i2s_channel_disable(s_rx_handle);
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    esp_err_t err = i2s_init(s_current_sample_rate_hz, on);
    xSemaphoreGive(s_i2s_mutex);
    if (err != ESP_OK) {
        logger_log(ESP_LOG_ERROR, TAG, "mic: falha ao %s o microfone: %s",
                   on ? "ligar" : "desligar", esp_err_to_name(err));
        return err;
    }

    s_mic_wanted = on;
    storage_set_i32(NVS_KEY_MIC_ENABLED, on ? 1 : 0);

    if (!on) {
        s_mic_adc_estado = MIC_ADC_MEDINDO;
        /* Avisa o rele que nao ha mais nada tocando por aqui.
         *
         * BUG corrigido em 2026-08-29: a passagem direta liga o amplificador
         * quando ha voz (relay_control_notify_playing(true)) e, ao desligar o
         * microfone, a task simplesmente PARAVA de rodar -- ninguem chamava
         * (false). Como o rele so desliga por um timer que e INICIADO por essa
         * chamada, o timer nunca comecava e o amplificador ficava ligado
         * indefinidamente. Medido: passou de 3 minutos com timeout de 120s,
         * sem audio e com o mic desligado, e o rele seguia acionado.
         *
         * Ainda nao incomodava porque o rele nao esta ligado no REM do
         * amplificador nesta bancada -- mas incomodaria assim que estivesse:
         * o amplificador nunca desligaria, e um amplificador ligado sem sinal
         * chia. */
        relay_control_notify_playing(false);
        logger_log(ESP_LOG_INFO, TAG, "mic: desligado (canal RX removido)");
        return ESP_OK;
    }

    /* A reaplicacao de ADCCONTROL2 que ficava aqui saiu: alem de
     * redundante (es8388_mic_config_begin() ja escreve a entrada), ela
     * escrevia SO o 0x0A e deixava o 0x0B com o valor anterior -- os dois
     * so fazem sentido em par. Ver es8388_mic_set_input_mode(). */
    vTaskDelay(pdMS_TO_TICKS(600));

    /* Mede em que estado o ADC subiu. Fora da faixa saudavel o audio do
     * microfone nao e misturado na saida, pra nao despejar chiado em cima da
     * musica.
     *
     * DUAS CORRECOES no detector (2026-08-30), depois de ele reprovar um ADC
     * que estava medido como PERFEITO (mediana 6, pico 124) e chama-lo de
     * "travado":
     *
     * 1. MEDIANA em vez de PICO. O criterio era o maior valor de 10 blocos --
     *    um unico estalo isolado bastava pra condenar uma captura limpa, e um
     *    pico e justamente o que aparece logo depois de acordar o codec. A
     *    diferenca real entre os dois estados e enorme e some na mediana:
     *    ADC bom da mediana 6-30, ADC ruidoso da ~1350. Sao 40x de separacao,
     *    contra picos que se sobrepoem. Este detector ja produziu falso
     *    negativo antes (8 boots bons reprovados em 2026-08-29) -- pela
     *    segunda vez o problema foi a estatistica escolhida, nao o limiar.
     *
     * 2. DESCARTA os primeiros blocos. Acordar o bloco digital do codec
     *    (0x02 = 0xF3 -> 0x00) e uma transicao brusca, e o buffer de DMA ainda
     *    guarda esse transiente quando a medicao comeca. */
    int16_t *amostra = heap_caps_malloc(MIC_LIVE_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    int32_t piso = 0;
    int32_t pico_bruto = 0;
    if (amostra != NULL) {
        const size_t MAX_MED = 512;
        int16_t *modulos = heap_caps_malloc(MAX_MED * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        size_t n_mod = 0;
        /* Descarta o transiente de acordar o codec, contando AMOSTRAS e nao
         * blocos: o tamanho do bloco ja mudou uma vez (1024 -> 512 bytes, para
         * reduzir a latencia) e levou junto, sem aviso, metade do descarte e
         * metade da janela de medicao -- foi assim que um ADC ruidoso passou a
         * ser classificado como "travado", porque a medicao acontecia antes de
         * o RX entregar a primeira amostra. */
        size_t descartadas = 0;
        while (descartadas < MIC_DETECT_DESCARTE && s_rx_handle != NULL) {
            size_t lidos = 0;
            if (i2s_channel_read(s_rx_handle, amostra, MIC_LIVE_CHUNK_BYTES, &lidos,
                                 pdMS_TO_TICKS(100)) != ESP_OK || lidos == 0) {
                break;
            }
            descartadas += lidos / sizeof(int16_t);
        }
        size_t medidas = 0;
        while (medidas < MIC_DETECT_JANELA && s_rx_handle != NULL) {
            size_t lidos = 0;
            if (i2s_channel_read(s_rx_handle, amostra, MIC_LIVE_CHUNK_BYTES, &lidos,
                                 pdMS_TO_TICKS(100)) != ESP_OK || lidos == 0) {
                break;
            }
            medidas += lidos / sizeof(int16_t);
            for (size_t i = 0; i + 1 < lidos / sizeof(int16_t); i += 2) {
                int32_t v = amostra[i] < 0 ? -(int32_t)amostra[i] : (int32_t)amostra[i];
                if (v > pico_bruto) {
                    pico_bruto = v;
                }
                if (modulos != NULL && n_mod < MAX_MED) {
                    modulos[n_mod++] = (int16_t)(v > INT16_MAX ? INT16_MAX : v);
                }
            }
        }
        if (modulos != NULL && n_mod > 0) {
            /* Ordenacao por insercao: n_mod <= 512 e isto roda uma vez por
             * subida do microfone, entao simplicidade vale mais que
             * velocidade aqui. */
            for (size_t i = 1; i < n_mod; i++) {
                int16_t chave = modulos[i];
                size_t j = i;
                while (j > 0 && modulos[j - 1] > chave) {
                    modulos[j] = modulos[j - 1];
                    j--;
                }
                modulos[j] = chave;
            }
            piso = modulos[n_mod / 2];
        }
        heap_caps_free(modulos);
        heap_caps_free(amostra);
    }

    const char *estado;
    if (piso == 0 && pico_bruto == 0) {
        s_mic_adc_estado = MIC_ADC_TRAVADO;
        estado = "TRAVADO -- nenhuma amostra; silenciado";
    } else if (piso <= MIC_PISO_MAX_SAUDAVEL) {
        s_mic_adc_estado = MIC_ADC_OK;
        estado = "saudavel";
    } else {
        s_mic_adc_estado = MIC_ADC_CHIANDO;
        estado = "CHIANDO -- silenciado";
    }
    logger_log(s_mic_adc_estado == MIC_ADC_OK ? ESP_LOG_INFO : ESP_LOG_WARN, TAG,
               "mic: ligado, ADC subiu %s (mediana=%d pico=%d)", estado, (int)piso, (int)pico_bruto);

    return ESP_OK;
}

/* Aplica o expansor ao bloco, in-place. Devolve true se algo audivel passou
 * (o rele do amplificador usa isso).
 *
 * Chamada nos DOIS caminhos do audio do microfone -- a mixagem com musica e a
 * passagem direta. Ja custou dois testes perdidos aplicar tratamento so num
 * deles: o Celio testava sem musica, entao o audio vinha pela passagem direta
 * e nada do que eu mexia na mixagem aparecia. */
static bool aplicar_expansor(int16_t *buf, size_t amostras, int32_t peak)
{
    (void)peak; /* a decisao nao usa mais o pico do bloco -- ver abaixo */

    if (!s_mic_auto_gate) {
        s_mic_gate_gain = MIC_GATE_ESCALA;
        return true;
    }

    const int32_t abrir = s_mic_gate_threshold;
    const int32_t fechar = abrir / 2; /* histerese */
    const int32_t faixa = (abrir > fechar) ? (abrir - fechar) : 1;

    int32_t g = s_mic_gate_gain;
    int32_t env = s_mic_envelope;

    for (size_t i = 0; i + 1 < amostras; i += 2) {
        int32_t v = buf[i] < 0 ? -(int32_t)buf[i] : (int32_t)buf[i];

        /* Envelope com ataque INSTANTANEO e queda lenta. E daqui que vem a
         * resposta imediata: a primeira amostra alta da palavra ja levanta o
         * envelope, sem esperar o bloco terminar. */
        if (v > env) {
            env = v;
        } else {
            env -= (env - v) >> 8; /* ~6ms de queda a 44,1kHz */
        }

        int32_t alvo;
        if (env >= abrir) {
            alvo = MIC_GATE_ESCALA;
        } else if (env <= fechar) {
            alvo = MIC_GATE_GANHO_MIN;
        } else {
            alvo = MIC_GATE_GANHO_MIN +
                   (int32_t)(((int64_t)(MIC_GATE_ESCALA - MIC_GATE_GANHO_MIN) *
                              (env - fechar)) / faixa);
        }

        const int32_t passo = (alvo > g) ? MIC_GATE_ATTACK_PASSO : MIC_GATE_RELEASE_PASSO;
        if (g < alvo) {
            g += passo;
            if (g > alvo) {
                g = alvo;
            }
        } else if (g > alvo) {
            g -= passo;
            if (g < alvo) {
                g = alvo;
            }
        }

        int16_t saida = (int16_t)(((int64_t)buf[i] * g) >> 16);
        buf[i] = saida;
        buf[i + 1] = saida;
    }

    s_mic_gate_gain = g;
    s_mic_envelope = env;
    return g > (MIC_GATE_GANHO_MIN * 2);
}

static void mic_live_task(void *arg);

static esp_err_t i2s_init(uint32_t sample_rate_hz, bool mic_enabled)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* Sem novo audio a tempo, o driver por padrao REPETE o ultimo conteudo
     * transmitido em vez de silencio -- candidato direto pro ruido tipo
     * "tique" repetitivo que persiste em idle. auto_clear_after_cb faz o
     * driver zerar (silencio de verdade) automaticamente nesse caso. */
    chan_cfg.auto_clear_after_cb = true;
    /* Mais descritores de DMA = mais folga contra qualquer engasgo breve do
     * escalonador de coexistencia WiFi/BT (que pode preemptar outros
     * perifericos por instantes durante eventos de radio).
     *
     * Com o mic ligado esse numero cai pela metade porque em full-duplex o
     * chan_cfg e UNICO: o RX herda exatamente o mesmo buffer do TX, entao 12
     * descritores custariam 2 x 11,5KB = 23KB de RAM interna. Sobrou 13,5KB
     * livres nessa configuracao -- e o Bluetooth sozinho consome ~11KB ao
     * conectar, o que derruba WiFi/HTTP/MQTT. Com 6 descritores o par
     * TX+RX volta a custar os ~11,5KB de antes do mic existir, e o TX ainda
     * fica com 6 x 240 = 1440 frames (~33ms a 44,1kHz) de folga. */
    /* Com o mic ligado eram 6 (metade), pra economizar RAM interna -- em
     * full-duplex o chan_cfg e unico e o RX herda o mesmo buffer do TX.
     *
     * Subido pra 10 em 2026-08-29: a captura crua do ADC mostrou que o que o
     * usuario ouve como "chiado" sao RAJADAS curtas (~0,4ms, ate 9169) sobre um
     * fundo limpissimo (mediana ~60). Elas correlacionam com ATIVIDADE DE
     * RADIO: sem trafego davam 0 por captura, e durante 8 capturas HTTP
     * seguidas subiram pra 2,5. E assinatura de amostra perdida -- o WiFi
     * preempta, o buffer do RX enche e a descontinuidade vira estalo. Subir a
     * prioridade da task (4->10) reduziu muito mas nao eliminou; mais buffer
     * da a folga que falta.
     *
     * REVERTIDO pra 6 em 2026-08-29: as metricas diziam melhora (rajadas
     * 2,5 -> 0,3 por captura, pico 5085 -> 2479), mas o Celio ouviu PIORA --
     * "quase constantes". O ouvido dele manda: a metrica que eu usava (contar
     * picos acima de 1500 numa captura de 46ms) claramente nao representa o
     * que se escuta. Buffer maior tambem adiciona ~25ms de latencia, ruim pra
     * karaoke. Nao repetir sem uma metrica validada contra a percepcao. */
    chan_cfg.dma_desc_num = mic_enabled ? 6 : 12;
    /* DESCRITORES MENORES QUANDO O MICROFONE ESTA LIGADO (2026-08-30).
     *
     * O que manda na latencia do microfone e o TEMPO de audio enfileirado, e
     * ele e dma_desc_num x dma_frame_num. Com o padrao de 240 frames por
     * descritor, 6 descritores sao 6 x 5,4ms = 32ms de fila -- no RX e no TX,
     * ou seja ~65ms entre cantar e ouvir.
     *
     * Isso passou a pesar depois que a passagem direta virou fluxo continuo
     * (ver o comentario sobre o `continue` removido em mic_live_task): antes a
     * fila do TX esvaziava nos silencios, agora ela fica sempre cheia -- que e
     * exatamente a condicao de latencia maxima. A correcao do estalo criou o
     * atraso, e o Celio ouviu os dois, um de cada vez.
     *
     * Metade dos frames por descritor corta o tempo de fila pela metade sem
     * mexer no NUMERO de descritores, que e o que protege contra um engasgo
     * pontual do escalonador de coexistencia WiFi/BT. A folga em tempo cai
     * junto, entao isto e reversivel: se voltar a engasgar na musica, este e o
     * primeiro numero a subir de novo.
     *
     * Fica so no caminho com microfone. Sem microfone a latencia nao importa
     * (ninguem esta cantando junto) e os 12 descritores cheios seguem sendo a
     * melhor protecao para Bluetooth e DLNA. */
    if (mic_enabled) {
        chan_cfg.dma_frame_num = 120;
    }

    /* TX e RX pedidos numa UNICA chamada -- este e o unico jeito de obter
     * full-duplex de verdade. A versao anterior alocava cada direcao em sua
     * propria chamada de i2s_new_channel() no mesmo I2S_NUM_0, cada uma como
     * I2S_ROLE_MASTER, e o comentario que estava aqui afirmava que o driver
     * "registra so a direcao pedida no mesmo controlador". Isso e FALSO no
     * ESP32. O guia do ESP-IDF (peripherals/i2s, secao Full-duplex Mode) diz:
     *
     *   "If both tx_handle and rx_handle are not NULL, it means this I2S
     *    controller will work at full-duplex mode"
     *   "For ESP32 and ESP32S2, the whole I2S controller (i.e. both RX and TX
     *    channel) will be occupied, even if only one of RX or TX channel is
     *    registered."
     *
     * Ou seja: alocar so o TX ja tomava o controlador inteiro em modo
     * simplex, e a segunda chamada (RX, tambem MASTER) devolvia ESP_OK sem
     * nunca formar um par -- dois masters disputando o mesmo BCLK/WS. O ADC
     * amostrava fora de fase com o ASDOUT do codec e entregava zeros exatos,
     * de forma intermitente conforme quem tivesse configurado a matriz de
     * GPIO por ultimo. Era essa a causa do microfone mudo, nao os
     * registradores do ES8388 (a busca por registrador "certo" nunca ia
     * convergir porque o problema estava do lado do ESP32).
     *
     * Custo assumido: em full-duplex o chan_cfg e unico, entao o RX herda os
     * 12 descritores do TX (~11,5KB de RAM interna) em vez dos ~1,5KB do
     * canal raso que existia antes. Nao da pra dar descritores diferentes por
     * direcao aqui. Baixar dma_desc_num tiraria do TX a folga que ele precisa
     * contra engasgo de coexistencia WiFi/BT, entao esse custo fica -- e so
     * existe quando o mic esta ligado (DEFAULT_MIC_ENABLED = 0). */
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_handle,
                                    mic_enabled ? &s_rx_handle : NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* din SEMPRE preenchido aqui (nao I2S_GPIO_UNUSED), mesmo com o mic
     * desligado: quando o RX e criado depois (mic_enabled), o driver exige
     * que o i2s_std_config_t do RX seja BYTE A BYTE identico ao do TX pra
     * reconhecer os dois como um par full-duplex de verdade (memcmp interno
     * do i2s_std.c) -- ver bloco do RX mais abaixo. Deixar os dois com os
     * mesmos pinos sempre evita esse descasamento (confirmado ao vivo: um
     * gpio_cfg diferente entre TX e RX derrubava audio_codec_init() com
     * ESP_ERR_INVALID_ARG, travando o boot inteiro em loop de crash). */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    /* APLL foi tentado aqui (clock dedicado de audio) e revertido: com
     * MCLK no GPIO0 nesta placa, causou o I2S travar silenciosamente
     * (ring buffer enchia, nenhum audio saia) -- provavel problema de
     * roteamento do APLL nesse pino especifico via este driver. Mantendo
     * o clock padrao (PLL_F160M via I2S_CLK_SRC_DEFAULT). */

    /* Os dois canais sao inicializados em std mode ANTES de qualquer enable,
     * e so entao habilitados em sequencia. O guia do ESP-IDF avisa que no
     * ESP32 varias configuracoes de TX/RX sao compartilhadas: "please make
     * sure they are working at same condition and under same status
     * (start/stop)". Habilitar o TX, seguir configurando o RX e so depois
     * habilitar o RX deixava os dois em status diferentes no meio do
     * caminho. */
    err = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (mic_enabled) {
        /* Mesmo std_cfg, byte a byte -- o i2s_std.c compara internamente as
         * duas configs e so trata o par como full-duplex se forem iguais. */
        err = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "falha ao inicializar canal I2S RX (mic): %s", esp_err_to_name(err));
            return err;
        }
    }

    /* ORDEM DOS ENABLES -- esta e a correcao da causa raiz do "ADC sobe
     * aleatoriamente saudavel, travado ou chiando" (2026-08-30).
     *
     * O que se fazia antes: habilitar o TX (o clock I2S nasce ai, porque o
     * ESP32 e o master), depois habilitar o RX, e so entao configurar o codec.
     * Isso tem dois defeitos que se somam, e os dois sao nao-deterministicos:
     *
     * 1. O RX era armado com o BCLK/WS JA CORRENDO. O guia do ESP-IDF avisa
     *    exatamente contra isso: no ESP32 as duas direcoes compartilham
     *    configuracao, e "please make sure they are working at same condition
     *    and under same status (start/stop)". Habilitado no meio de um frame,
     *    o receptor engata num ponto arbitrario -- e um deslocamento de bits
     *    na amostra de 16 bits e ruido de banda larga com amplitude enorme,
     *    ou seja, exatamente o "chiado".
     *
     * 2. O bloco digital e a DLL do codec eram acordados (reg 0x02) com o
     *    clock correndo ha segundos. A secao 5.1 do ES8388 User Guide diz que
     *    "in slave mode, ES8388 can auto check MCLK/LRCK ratio and MCLK/SCLK
     *    ratio" -- o codec MEDE os clocks nesse instante. Medindo no meio de
     *    um frame qualquer, as vezes acerta e as vezes nao.
     *
     * Isso fecha com tudo que foi medido e nunca teve explicacao: por que a
     * taxa de sucesso variava de boot para boot sem nada mudar, por que
     * NENHUMA recuperacao em runtime funcionava (reescrever registradores,
     * resetar a maquina de estados, resetar o chip, destruir e recriar os
     * canais -- sete tentativas), e por que ligar o microfone com o sistema em
     * carga falhava quase sempre: a janela entre um enable e o outro fica
     * maior e mais variavel.
     *
     * A ordem correta e determinista:
     *   1. configurar o codec inteiro com o clock PARADO (config_begin, que
     *      deixa STM/DLL/digital parados em 0x02=0xF3);
     *   2. armar o RX ANTES de existir clock, para ele engatar no primeiro
     *      frame em vez de no meio de um;
     *   3. habilitar o TX -- e aqui que MCLK/BCLK/WS nascem, ja com os dois
     *      lados prontos e esperando;
     *   4. so entao acordar o bloco digital do codec (config_end), com o
     *      clock estavel para a auto-deteccao medir. */
    if (mic_enabled) {
        err = es8388_mic_config_begin();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "falha ao configurar ADC/microfone do ES8388: %s", esp_err_to_name(err));
            return err;
        }
        err = i2s_channel_enable(s_rx_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "falha ao habilitar canal I2S RX (mic): %s", esp_err_to_name(err));
            return err;
        }
    }

    err = i2s_channel_enable(s_tx_handle);
    if (err != ESP_OK) {
        return err;
    }

    if (mic_enabled) {
        err = es8388_mic_config_end();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "falha ao acordar o bloco digital do ES8388: %s", esp_err_to_name(err));
            return err;
        }

        /* Buffers de mixagem (karaokê) -- ver write_with_mic_mix(). PSRAM:
         * sobra de sobra (~4MB livres) e nada aqui precisa da RAM interna,
         * que é o recurso realmente escasso deste firmware. */
        /* So aloca se ainda nao existir: i2s_init() e chamada de novo pela
         * auto-recuperacao do microfone (ver mic_live_task), e realocar aqui
         * vazaria PSRAM a cada tentativa. */
        if (s_mic_mix_music_buf == NULL) {
            s_mic_mix_music_buf = heap_caps_malloc(MIC_MIX_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
        }
        if (s_mic_mix_read_buf == NULL) {
            s_mic_mix_read_buf = heap_caps_malloc(MIC_MIX_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
        }
        if (s_mic_mix_music_buf == NULL || s_mic_mix_read_buf == NULL) {
            ESP_LOGE(TAG, "falha ao alocar buffers de mixagem do mic (PSRAM)");
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t audio_codec_init(void)
{
    /* O log DEBUG do driver I2S foi usado aqui (2026-08-29) pra investigar o
     * boot pela serial. Rendeu: provou que os logs do driver sao IDENTICOS
     * entre um boot saudavel e um travado, descartando o I2S como culpado, e
     * mostrou que o codec era configurado 2ms ANTES do I2S existir (sem MCLK).
     * Removido depois disso -- so custava flash e trafego na serial. Pra
     * reativar: esp_log_level_set("i2s_common"/"i2s_std", ESP_LOG_VERBOSE) e
     * CONFIG_LOG_MAXIMUM_LEVEL=4 no sdkconfig (senao o DEBUG sai em tempo de
     * compilacao e o ajuste em runtime nao faz nada). */
    s_i2s_mutex = xSemaphoreCreateMutex();

    /* ORDEM INVERTIDA (2026-08-29): I2S ANTES do codec.
     *
     * O ES8388 era configurado inteiro ANTES do I2S existir -- ou seja, com
     * MCLK AUSENTE. Capturado no serial (log DEBUG do driver, ideia do Celio
     * de investigar pela serial):
     *
     *   I (2288) es8388:     ES8388 inicializado (DAC/line-out...)
     *   D (2290) i2s_common: tx channel is registered on I2S0
     *
     * O codec recebia clock/modo escravo/alimentacao 2ms antes de haver
     * qualquer clock mestre chegando nele. Um codec configurado sem MCLK tem
     * comportamento indeterminado, e e exatamente isso que observamos: o ADC
     * sobe saudavel, travado ou chiando de forma aleatoria (~35% saudavel em
     * ~45 boots).
     *
     * O serial tambem DESCARTOU o culpado que eu vinha perseguindo: os logs do
     * driver I2S sao BYTE A BYTE IDENTICOS entre um boot bom e um ruim (mesmo
     * mclk 11289537Hz, mesmo bclk, mesma sequencia, mesmos buffers). O canal RX
     * engata igual sempre -- o problema nunca esteve no lado do ESP32.
     *
     * Agora: sobe o I2S primeiro (o que liga o MCLK no GPIO0) e so entao
     * configura o codec, que e a ordem que os drivers de referencia usam. */
    esp_err_t err = i2s_init(44100, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao inicializar o I2S: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50)); /* deixa o MCLK estabilizar antes do I2C */

    err = es8388_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao inicializar o ES8388: %s", esp_err_to_name(err));
        return err;
    }

    /* Entrada analogica ANTES de qualquer coisa ligar o ADC: quem escreve
     * 0x0A/0x0B de verdade e es8388_mic_config_begin(), e ele usa o valor
     * memorizado aqui. */
    int32_t mic_input = DEFAULT_MIC_INPUT;
    storage_get_i32(NVS_KEY_MIC_INPUT, &mic_input, DEFAULT_MIC_INPUT);
    es8388_mic_set_input_mode((es8388_mic_input_t)mic_input);

    int32_t mic_enabled = DEFAULT_MIC_ENABLED;
    storage_get_i32(NVS_KEY_MIC_ENABLED, &mic_enabled, DEFAULT_MIC_ENABLED);
    s_mic_wanted = (mic_enabled != 0);

    /* INICIALIZACAO DO MIC POSTERGADA (ideia do Celio, 2026-08-28).
     *
     * O canal I2S RX NAO e criado aqui. No boot ele subia junto com WiFi, BT,
     * SPIFFS e MQTT inicializando ao mesmo tempo, e o ADC saía travado
     * (entregando so zeros) na maioria das vezes -- medido reiniciando o
     * aparelho varias vezes seguidas: 4 em 5 boots travados antes da correcao
     * da sequencia de energizacao, e ainda ~2 em 3 depois dela.
     *
     * Nenhuma recuperacao em runtime resolve depois que trava: reescrever os
     * registradores do ADC, resetar a maquina de estados do codec, resetar o
     * chip inteiro, desabilitar/reabilitar o canal I2S e ate destruir e
     * recriar os canais -- todas testadas, todas devolvendo ESP_OK e o piso
     * seguindo em 0. So o reset do ESP32 recuperava, o que aponta pra estado
     * do periferico I2S abaixo do que a API do driver alcanca.
     *
     * Dai a estrategia: em vez de recuperar depois, EVITAR a corrida. Sobe so
     * o TX aqui (audio de saida funciona desde o primeiro instante) e a task
     * do microfone promove o canal para full-duplex alguns segundos depois,
     * com o sistema ja estavel. Ver mic_live_task. */
    int32_t saved_volume = DEFAULT_VOLUME_USER;
    storage_get_i32(NVS_KEY_VOLUME_USER, &saved_volume, DEFAULT_VOLUME_USER);
    audio_codec_set_volume((int)saved_volume);

    int32_t mic_auto_gate = DEFAULT_MIC_AUTO_GATE;
    storage_get_i32(NVS_KEY_MIC_AUTO_GATE, &mic_auto_gate, DEFAULT_MIC_AUTO_GATE);
    s_mic_auto_gate = (mic_auto_gate != 0);

    int32_t mic_gain = DEFAULT_MIC_GAIN;
    storage_get_i32(NVS_KEY_MIC_GAIN, &mic_gain, DEFAULT_MIC_GAIN);
    s_mic_digital_gain_x100 = mic_gain * 4;

    int32_t gate_level = MIC_GATE_THRESHOLD;
    storage_get_i32(NVS_KEY_MIC_GATE_LEVEL, &gate_level, MIC_GATE_THRESHOLD);
    s_mic_gate_threshold = gate_level;

    /* Passagem direta do microfone quando nao ha musica (ver mic_live_task).
     * Prioridade baixa: e audio "de conveniencia", nunca deve competir com a
     * reproducao de verdade. So sobe se o microfone estiver habilitado. */
    /* Task criada SEMPRE: ela e quem liga o microfone (no boot, se habilitado)
     * e quem faz a passagem direta. Com o microfone desligado ela so dorme.
     *
     * PRIORIDADE 10 (era 4). Diagnostico de 2026-08-29, com a captura crua do
     * ADC (/api/mic/raw): o que o usuario ouvia como "chiado" NAO e ruido
     * continuo -- o piso e limpissimo (mediana 54). Sao RAJADAS esporadicas,
     * curtas (~0,4ms) e altas (ate 9169), em 2 de 6 capturas. Isso e assinatura
     * de amostra perdida no buffer do I2S: a task lia em prioridade baixa e,
     * preemptada por WiFi/BT, deixava o buffer do RX encher -- e a
     * descontinuidade vira estalo. Casa com o relato do usuario de que o
     * chiado "nao e constante".
     *
     * 10 fica acima das tasks de rede comuns e abaixo do stack de radio, entao
     * o microfone e servido a tempo sem competir com o audio de verdade. */
    {
        if (xTaskCreate(mic_live_task, "mic_live", 3072, NULL, 10, NULL) != pdPASS) {
            ESP_LOGE(TAG, "falha ao criar task do microfone (passagem direta)");
        }
    }

    ESP_LOGI(TAG, "audio_codec pronto (I2S MCLK=%d BCLK=%d WS=%d DOUT=%d)",
             PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT);
    return ESP_OK;
}

/* Curva linear EM dB, não quadrática sobre a fração do slider: dB já É uma
 * escala perceptual (é assim que potenciômetros de áudio "de verdade" são
 * calibrados — tantos dB por grau de rotação), então aplicar x² por cima
 * dobra a compressão. Isso causava um bug real: es8388_set_volume(0-100)
 * atenua linearmente até -96dB, e x² jogava a metade do slider pra -72dB
 * (já inaudível na prática) — só os últimos ~20% do curso faziam
 * diferença perceptível. Restringimos a faixa útil a só os últimos
 * VOLUME_MAX_ATTEN_DB dB (abaixo disso já é imperceptível/mascarado por
 * ruído ambiente de qualquer forma; mudo de verdade é audio_codec_set_mute).
 * Era 50dB, depois 30dB (relato antigo: "em nível baixo o ganho é pouco",
 * com o amplificador de então). 2026-08-22, já com o amplificador externo
 * real/mais potente (ver também o corte fixo de ~15dB na saída analógica em
 * es8388.c): 30dB não bastava mais -- o fundo da barra (posições 2, 10...)
 * soava praticamente igual, e já alto. Como esta curva é linear em dB sobre
 * a posição normalizada, o dB por passo é VOLUME_MAX_ATTEN_DB/VOLUME_STEPS
 * -- 30dB/200 passos é só ~0.15dB/passo, quase imperceptível entre posições
 * próximas do zero. Subido pra 60dB: o topo da barra (volume máximo) não
 * muda (normalized=1 sempre cai em 0dB de atenuação digital, e o corte
 * analógico fixo continua igual), só o fundo fica bem mais silencioso e com
 * mais resolução -- ajustar de novo se ainda não for suficiente. */
#define VOLUME_MAX_ATTEN_DB 60.0f

static esp_err_t apply_curve_and_write(int volume_0_to_steps)
{
    if (volume_0_to_steps < 0) {
        volume_0_to_steps = 0;
    } else if (volume_0_to_steps > VOLUME_STEPS) {
        volume_0_to_steps = VOLUME_STEPS;
    }
    float normalized = (float)volume_0_to_steps / VOLUME_STEPS; /* 0.0-1.0 */
    /* es8388_set_volume(0-100) mapeia 0->0dB e 100->-96dB linearmente;
     * escolhemos es_vol pra restringir isso aos últimos VOLUME_MAX_ATTEN_DB
     * dB desse range em vez do range inteiro. */
    float es_vol_f = 100.0f - (VOLUME_MAX_ATTEN_DB / 0.96f) * (1.0f - normalized);
    uint8_t es_vol = (uint8_t)(es_vol_f < 0.0f ? 0.0f : es_vol_f);
    return es8388_set_volume(es_vol);
}

esp_err_t audio_codec_set_volume(int volume)
{
    esp_err_t err = apply_curve_and_write(volume);
    if (err == ESP_OK) {
        if (volume < 0) {
            volume = 0;
        } else if (volume > VOLUME_STEPS) {
            volume = VOLUME_STEPS;
        }
        /* So grava na NVS se o valor de fato mudou -- achado ao vivo
         * (2026-08-16, era do Slimproto, ja removido -- ver
         * docs/slimproto_retrospective.md): um "audg" (sincronizacao de
         * volume vinda do servidor) chamava esta funcao toda vez, MESMO
         * quando o volume nao mudou de verdade -- gravando na
         * NVS sem necessidade a cada vez. Uma escrita na flash desliga o
         * cache por um instante; se isso coincidir com outra task tocando
         * PSRAM (nossos ring buffers de audio) ou no meio de uma operacao
         * de rede (lwIP), da exatamente o tipo de crash "Cache disabled but
         * cached memory region accessed" capturado ao vivo essa noite
         * durante uma rajada de pulo de faixa. Nao elimina o mecanismo por
         * completo (uma mudanca de volume real ainda grava), mas corta a
         * fonte mais frequente e desnecessaria dessas escritas. */
        if (volume != s_current_volume) {
            s_current_volume = volume;
            storage_set_i32(NVS_KEY_VOLUME_USER, volume);
        }
        bt_audio_notify_volume_changed(volume);
    }
    return err;
}

esp_err_t audio_codec_apply_gain(int volume)
{
    return apply_curve_and_write(volume);
}

int audio_codec_get_volume(void)
{
    return s_current_volume;
}

esp_err_t audio_codec_set_mute(bool mute)
{
    return es8388_set_mute(mute);
}

static inline int16_t clamp_s16(int32_t v)
{
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)v;
}

/* Karaokê: mistura o PCM do microfone por cima do que está prestes a ir pro
 * DAC. Fica aqui (não em bt_audio.c/dlna_renderer.c) de propósito -- os
 * dois já convergem pra audio_codec_write(), então mixar aqui cobre
 * qualquer fonte (Bluetooth ou DLNA) sem duplicar lógica em cada uma.
 * Processa em blocos de MIC_MIX_CHUNK_BYTES porque os buffers de mixagem
 * têm tamanho fixo, mas quem chama manda blocos de tamanho variável (ring
 * buffer do BT, chunks do decoder DLNA/FLAC). */
static esp_err_t write_with_mic_mix(const uint8_t *data, size_t len, size_t *bytes_written)
{
    size_t total_written = 0;
    esp_err_t err = ESP_OK;

    while (total_written < len) {
        size_t chunk = len - total_written;
        if (chunk > MIC_MIX_CHUNK_BYTES) {
            chunk = MIC_MIX_CHUNK_BYTES;
        }
        chunk -= chunk % 4; /* múltiplo de 4 bytes -- 1 frame estéreo 16 bits */
        if (chunk == 0) {
            break;
        }

        memcpy(s_mic_mix_music_buf, data + total_written, chunk);

        /* Timeout ZERO (nao-bloqueante): se o mic nao tiver amostra pronta
         * NESTE INSTANTE, essa rodada so mixa silencio -- a reproducao NUNCA
         * pode ficar refem do microfone. Era pdMS_TO_TICKS(5): relato do
         * usuario 2026-08-22, picotes "como se o buffer estivesse cheio"
         * com o mic ligado -- cada write() de audio agora podia esperar ate
         * 5ms so pra essa leitura extra do RX, exatamente empilhando em
         * cima de qualquer engasgo ja existente da coexistencia WiFi/BT (o
         * TX ja luta contra isso, ver dma_desc_num=12 acima). Com o RX tendo
         * DMA proprio rodando continuamente (3 descritores de 128 frames),
         * a amostra normalmente ja esta pronta -- 0 corta essa espera extra
         * por completo sem trocar o comportamento em silencio. */
        size_t mic_bytes = 0;
        i2s_channel_read(s_rx_handle, s_mic_mix_read_buf, chunk, &mic_bytes, 0);

        int16_t *music = (int16_t *)s_mic_mix_music_buf;
        int16_t *mic_mut = (int16_t *)s_mic_mix_read_buf;
        const int16_t *mic = mic_mut;
        size_t mic_samples = mic_bytes / sizeof(int16_t);

        /* MONO (2026-08-28): o microfone desta placa e a entrada
         * DIFERENCIAL MIC1 do ES8388 -- ou seja, UM canal so. Ler em estereo
         * traz um segundo canal que nao tem microfone nenhum: so ruido de
         * fundo do ADC. Misturar esse canal no audio jogava chiado puro na
         * saida (relato "chiado" persistente mesmo com ganho baixo). Agora
         * usamos so o canal esquerdo e duplicamos pros dois -- metade do
         * ruido some de imediato e a voz fica centralizada. */
        int32_t peak = 0;
        /* Estado do passa-baixa preservado entre blocos -- senao o filtro
         * reiniciaria a cada chamada e produziria um "clique" em cada
         * fronteira de bloco. */
        static int32_t lpf_y;
        /* Janela do supressor de impulso, preservada entre blocos. */
        static int16_t m1, m2;
        for (size_t i = 0; i + 1 < mic_samples; i += 2) {
            /* 1) mata impulsos de 1 amostra (ver mediana3); 2) passa-baixa. */
            int16_t bruta = mic_mut[i];
            int32_t esq = mediana3(m1, m2, bruta);
            m1 = m2;
            m2 = bruta;
            lpf_y += ((esq - lpf_y) * MIC_LPF_K) >> 8;
            esq = lpf_y;
            mic_mut[i] = (int16_t)esq;
            mic_mut[i + 1] = (int16_t)esq; /* direito recebe o esquerdo (mono) */
            int32_t v = esq < 0 ? -esq : esq;
            if (v > peak) {
                peak = v;
            }
        }
        s_mic_peak = (int16_t)(peak > INT16_MAX ? INT16_MAX : peak);

        /* Portao com HISTERESE (2026-08-28): antes era um limiar unico, que
         * cortava silabas -- a voz oscila muito dentro de uma frase e o
         * portao abria/fechava no meio das palavras. Agora abre num limiar
         * mais alto e so fecha depois de um tempo abaixo de um limiar mais
         * baixo, que e como um noise gate de verdade funciona. */
        /* Expansor com rampa em vez do corte binario que ficava aqui --
         * ver aplicar_expansor(). O que havia antes zerava mic_samples de uma
         * vez, o que e um degrau: entrava e saia som instantaneamente, com o
         * piso do receptor junto. */
        aplicar_expansor(mic_mut, mic_samples, peak);

        /* Ganho digital do microfone, ajustavel em tempo real (0-100 ->
         * 0..~4x). O ganho analogico do PGA no ES8388 tem passos grossos de
         * ~3dB e amplifica o ruido junto; um ganho digital aqui permite
         * afinar fino sem mexer no hardware -- e, combinado com o portao
         * acima, sobe a voz sem trazer o chiado do silencio junto. */
        int32_t g = s_mic_digital_gain_x100;
        for (size_t i = 0; i < mic_samples; i++) {
            int32_t amostra = ((int32_t)mic[i] * g) / 100;
            music[i] = clamp_s16((int32_t)music[i] + amostra);
        }

        size_t chunk_written = 0;
        /* Mesmo motivo do prazo em audio_codec_write() -- este caminho roda
         * com o mutex do I2S tomado, entao uma escrita presa aqui congelaria
         * a outra fonte de audio igual. */
        err = i2s_channel_write(s_tx_handle, s_mic_mix_music_buf, chunk, &chunk_written, portMAX_DELAY);
        total_written += chunk_written;
        if (err != ESP_OK || chunk_written < chunk) {
            break;
        }
    }

    if (bytes_written != NULL) {
        *bytes_written = total_written;
    }
    return err;
}

/* Timestamp da ultima escrita de audio real no I2S -- unico ponto por onde
 * BT (bt_audio.c) e DLNA (dlna_renderer.c) passam pra tocar de verdade.
 * Usado por um watchdog (dlna_renderer.c) que detecta "estado diz tocando,
 * mas nada esta saindo de verdade" e reage, em vez de deixar o usuario preso
 * ouvindo silencio sem saber. */
/* Sinaliza que audio_codec_reconfigure_clock() esta tentando pegar o mutex
 * do I2S -- as tasks de I2S (BT e DLNA), que rodam em prioridade bem mais
 * alta, checam isso e cedem, senao a reconfiguracao nunca consegue entrar
 * (inanicao por prioridade -- ver comentario la). */
static volatile bool s_clock_change_pending = false;

bool audio_codec_clock_change_pending(void)
{
    return s_clock_change_pending;
}

/* -------------------------------------------------------------------------
 * Microfone SEMPRE ativo (nao so "karaoke")
 *
 * Pedido do usuario 2026-08-28: "pode simplificar e deixar ele sempre fluir,
 * com ou sem audio tocando". Ate aqui o microfone so existia DENTRO de
 * write_with_mic_mix(), que por sua vez so roda quando uma fonte (BT/DLNA)
 * esta enviando musica -- sem musica, o microfone nem era lido.
 *
 * Esta task cobre o caso "sem musica": le o microfone continuamente e manda
 * direto pro I2S. Quando ha musica tocando, ela sai do caminho e deixa a
 * mixagem cuidar (senao os dois escreveriam no mesmo canal, brigando).
 * Detecta isso pelo timestamp da ultima escrita de musica -- o mesmo
 * indicador ja usado pelo watchdog.
 * ------------------------------------------------------------------------- */

/* Se houve escrita de musica ha menos que isso, a mixagem esta ativa e esta
 * task nao escreve nada. */
#define MIC_LIVE_MUSIC_IDLE_US 150000

static void mic_live_task(void *arg)
{
    (void)arg;
    int16_t *buf = heap_caps_malloc(MIC_LIVE_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "mic: sem PSRAM pro buffer da passagem direta -- microfone so funcionara com musica");
        vTaskDelete(NULL);
        return;
    }

    bool desmutado_por_nos = false;
    int blocos_sem_voz = 0;
    bool relay_ligado_por_nos = false;
    logger_log(ESP_LOG_INFO, TAG, "mic: task de passagem direta iniciada");

    /* Reaplica a selecao de entrada do ADC com o sistema ja rodando.
     *
     * ATENCAO ao historico: o comentario que estava aqui afirmava que a
     * escrita feita dentro de es8388_mic_init() "nao pega" e que so a
     * reaplicacao tardia fazia a captura funcionar. Essa conclusao era
     * INVALIDA -- veio de comparar leituras do medidor de pico, que congela no
     * ultimo valor quando o ADC nao entrega dados novos, entao valores velhos
     * foram lidos como se fossem medicoes frescas. A causa real do "ADC sai do
     * boot entregando zeros" era o I2S alocado como dois canais simplex em vez
     * de um par full-duplex (ver i2s_init()), o que tornava a captura
     * intermitente independente de qualquer registrador do codec.
     *
     * A reaplicacao fica porque e idempotente e barata, nao porque esteja
     * provado que faz falta. Se depois de validado o full-duplex ela puder
     * sair, sai -- mas nao no mesmo passo da correcao estrutural, pra nao
     * mexer em duas variaveis de uma vez. */
    /* Espera o sistema estabilizar antes de criar o canal RX -- ver o
     * comentario em audio_codec_init(). 12s cobre com folga WiFi, MQTT, SPIFFS
     * e o anuncio de mDNS/DLNA, que e onde a disputa acontecia. */
    vTaskDelay(pdMS_TO_TICKS(12000));

    /* Promove o canal para full-duplex e CONFERE o resultado, repetindo se o
     * ADC subir ruim.
     *
     * Medido em 6 boots com a promocao postergada (2026-08-28): 5 sobem
     * limpos, com piso notavelmente consistente (42, 43, 44, 45, 45), e 1 sobe
     * chiando (~5979). Os boots travados em zero, que eram a maioria quando o
     * canal nascia junto do boot, sumiram. Como a promocao agora acontece com
     * o sistema parado, ela pode simplesmente ser refeita quando o resultado
     * nao presta -- coisa impossivel durante a inicializacao.
     *
     * Faixa saudavel: o ADC bom entrega um piso pequeno mas NAO nulo. Zero
     * cravado = canal nao engatou; alto = subiu chiando. */
    /* Liga o microfone se ele estiver habilitado no NVS. Toda a mecanica de
     * criar/derrubar o canal vive em audio_codec_mic_set_enabled(), que
     * tambem atende os acionamentos em runtime (KEY2, web, API, MQTT). */
    if (s_mic_wanted) {
        audio_codec_mic_set_enabled(true);
    }

    /* Orcamento de correcoes por corte de MCLK. Reposto sempre que o conversor
     * volta a ficar bom, para que um problema futuro tenha as tentativas
     * inteiras de novo. */
    int cortes_mclk = 0;

    int diag_ciclos = 0;
    int zeros_seguidos = 0;
    int tentativas_recuperacao = 0;
    int64_t diag_ultimo_us = 0;

    for (;;) {
        /* Diagnostico 2026-08-28: o pico do mic ficou 0 mesmo com o usuario
         * falando, e nao havia como saber se esta task estava rodando, se a
         * leitura falhava ou se ela nem chegava a ler. Resumo a cada 5s. */
        {
            int64_t agora_diag = esp_timer_get_time();
            /* Era 5s. O buffer de log e circular e este resumo sozinho o
             * enchia: num teste de 2026-08-30 ele apagou justamente a janela
             * que eu queria ler. 30s acompanha igual e nao atropela o resto. */
            if (agora_diag - diag_ultimo_us > 30000000) {
                diag_ultimo_us = agora_diag;
                /* min/max separados (2026-08-28): o "pico" sozinho enganava.
                 * Som e VARIACAO -- se o ADC entregar um valor constante
                 * (nivel continuo), o pico aparece alto e mesmo assim nao
                 * sai som nenhum. min~=max => sinal continuo, nao audio. */
                logger_log(ESP_LOG_INFO, TAG, "mic: %d ciclos, pico=%d, min=%d max=%d (amplitude=%d)",
                           diag_ciclos, (int)s_mic_peak, (int)s_mic_min, (int)s_mic_max,
                           (int)(s_mic_max - s_mic_min));
                diag_ciclos = 0;
            }
        }
        diag_ciclos++;

        if (s_rx_handle == NULL) {
            vTaskDelay(pdMS_TO_TICKS(500)); /* microfone desligado nesta sessao */
            continue;
        }
        /* CONVERSOR RUIM: corrige, em vez de so silenciar.
         *
         * Isto vivia ANTES do laco e rodava uma unica vez, logo apos a subida
         * do microfone. Bastava o conversor ficar ruim depois disso -- um
         * corte de MCLK que sai ruim, por exemplo, ja que cada um vale por uma
         * nova tentativa -- para o firmware ficar mudo ate alguem reiniciar o
         * aparelho, que e exatamente o que esta correcao existe para evitar.
         *
         * Continua NAO sendo "tentar ate dar certo": e a acao especifica que
         * corrige a causa medida (o conversor precisa ver o MCLK morrer, ver
         * audio_codec_mic_hard_reset), com teto e com conferencia do resultado.
         * Enquanto o conversor estiver ruim o audio do microfone segue
         * silenciado -- melhor mudo que chiando. */
        if (s_mic_adc_estado != MIC_ADC_OK && s_mic_adc_estado != MIC_ADC_MEDINDO) {
            if (cortes_mclk < MIC_MAX_HARD_RESETS) {
                cortes_mclk++;
                logger_log(ESP_LOG_WARN, TAG,
                           "mic: conversor ruim -- cortando o MCLK pra corrigir (%d de %d)",
                           cortes_mclk, MIC_MAX_HARD_RESETS);
                s_recuperacao_em_curso = true;
                audio_codec_mic_hard_reset();
                s_recuperacao_em_curso = false;
                continue;
            }
            if (desmutado_por_nos) {
                relay_control_notify_playing(false);
                desmutado_por_nos = false;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        cortes_mclk = 0; /* conversor bom: devolve o orcamento inteiro */
        /* Musica tocando? Entao a mixagem ja cuida do microfone. */
        if ((esp_timer_get_time() - s_last_write_us) < MIC_LIVE_MUSIC_IDLE_US) {
            desmutado_por_nos = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t lidos = 0;
        esp_err_t rd = i2s_channel_read(s_rx_handle, buf, MIC_LIVE_CHUNK_BYTES, &lidos, pdMS_TO_TICKS(100));
        if (rd != ESP_OK || lidos < 4) {
            static int64_t erro_ultimo_us;
            int64_t agora_e = esp_timer_get_time();
            if (agora_e - erro_ultimo_us > 5000000) {
                erro_ultimo_us = agora_e;
                logger_log(ESP_LOG_WARN, TAG, "mic: leitura falhou (%s, lidos=%u)",
                           esp_err_to_name(rd), (unsigned)lidos);
            }
            continue;
        }
        size_t amostras = lidos / sizeof(int16_t);

        /* Mono + supressor de impulso + passa-baixa + pico.
         *
         * ATENCAO: existem DOIS caminhos pro audio do microfone -- a mixagem
         * (write_with_mic_mix, quando ha musica tocando) e ESTA passagem
         * direta (quando nao ha). Os filtros foram aplicados so na mixagem
         * primeiro, e como o usuario testava SEM musica, o audio vinha por
         * aqui CRU e nada mudava. Qualquer tratamento novo tem que entrar nos
         * dois lugares. */
        int32_t peak = 0;
        int16_t vmin = INT16_MAX, vmax = INT16_MIN;
        static int16_t lm1, lm2;
        static int32_t llpf;
        for (size_t i = 0; i + 1 < amostras; i += 2) {
            int16_t bruta = buf[i];
            int32_t f = mediana3(lm1, lm2, bruta);
            lm1 = lm2;
            lm2 = bruta;
            llpf += ((f - llpf) * MIC_LPF_K) >> 8;
            int16_t esq = (int16_t)llpf;
            buf[i] = esq;
            buf[i + 1] = esq;
            if (esq < vmin) vmin = esq;
            if (esq > vmax) vmax = esq;
            int32_t v = esq < 0 ? -(int32_t)esq : (int32_t)esq;
            if (v > peak) {
                peak = v;
            }
        }
        s_mic_min = vmin;
        s_mic_max = vmax;
        s_mic_peak = (int16_t)(peak > INT16_MAX ? INT16_MAX : peak);


        /* TRAVA DURANTE O USO -- distinta da que age na subida do microfone.
         *
         * O que estava aqui destruia e recriava os canais I2S depois de 2s de
         * zero absoluto. Duas razoes para trocar:
         *
         * 1. A ACAO NAO FUNCIONA. Recriar os canais foi testado 18 vezes em
         *    2026-08-28 sem nunca recuperar, e reconfirmado em 2026-08-30 no
         *    isolamento completo. O que recupera e cortar o MCLK -- ver
         *    audio_codec_mic_hard_reset().
         * 2. O GATILHO FICOU PERIGOSO. Ele disparava com 2s de pico zero,
         *    calibrado quando o piso de ruido era alto e zero absoluto era
         *    inequivoco. Com a entrada correta selecionada o piso caiu para
         *    mediana ~6, e depois do passa-baixa o pico pode legitimamente dar
         *    0 num silencio de verdade -- o firmware passaria a derrubar o
         *    audio sozinho, sem defeito nenhum.
         *
         * Agora: 15s (nao 2s) de zero absoluto, e a acao e o corte de MCLK. */
        if (peak == 0) {
            zeros_seguidos++;
            if (zeros_seguidos > 2580) { /* ~172 blocos/s -> ~15s */
                zeros_seguidos = 0;
                if (tentativas_recuperacao < MIC_MAX_TENTATIVAS_RECUPERACAO) {
                    tentativas_recuperacao++;
                    logger_log(ESP_LOG_WARN, TAG,
                               "mic: silencio absoluto ha ~15s -- cortando o MCLK "
                               "(tentativa %d de %d)",
                               tentativas_recuperacao, MIC_MAX_TENTATIVAS_RECUPERACAO);
                    s_recuperacao_em_curso = true;
                    audio_codec_mic_hard_reset();
                    s_recuperacao_em_curso = false;
                } else if (tentativas_recuperacao == MIC_MAX_TENTATIVAS_RECUPERACAO) {
                    tentativas_recuperacao++; /* passa do limite: nao loga mais */
                    logger_log(ESP_LOG_ERROR, TAG,
                               "mic: ADC continua mudo apos %d cortes de MCLK -- "
                               "desistindo. Verificar hardware.",
                               MIC_MAX_TENTATIVAS_RECUPERACAO);
                }
            }
        } else {
            zeros_seguidos = 0;
            /* Voltou a entregar dado: zera o contador pra que um travamento
             * futuro tenha o orcamento de tentativas de novo. */
            tentativas_recuperacao = 0;
        }

        /* Mesmo expansor da mixagem -- os dois caminhos TEM que receber o
         * mesmo tratamento. Ver o comentario em aplicar_expansor(). */
        bool passar = aplicar_expansor(buf, amostras, peak);

        /* NAO EXISTE MAIS "portao fechado, pula o bloco".
         *
         * O que estava aqui dava `continue` quando o portao fechava, ou seja,
         * a task PARAVA DE ESCREVER no I2S. Duas consequencias, e a segunda e
         * exatamente o defeito que o Celio descreveu ("falei 1, 2, 3 testando
         * e chia no inicio de cada um"):
         *
         *  - o bloco que o expansor tinha acabado de atenuar era descartado,
         *    entao a rampa de descida nunca era ouvida: o audio sumia de
         *    uma vez, que e o degrau que a rampa existia para evitar;
         *  - o fluxo para o I2S ficava com buracos. Retomar a escrita depois
         *    de um vazio e uma descontinuidade na forma de onda, e
         *    descontinuidade e estalo -- um em cada palavra.
         *
         * Agora o bloco e SEMPRE escrito. No silencio ele vai atenuado em
         * -30dB, o que com o piso de 1400 do receptor da ~44 antes do ganho
         * digital: inaudivel, e o fluxo nunca quebra. */

        int32_t g = s_mic_digital_gain_x100;
        for (size_t i = 0; i < amostras; i++) {
            buf[i] = clamp_s16(((int32_t)buf[i] * g) / 100);
        }

        /* DESMUTA ANTES da voz chegar, nao quando ela chega.
         *
         * O unmute do DAC e uma ida ao I2C (leitura + escrita, com retry) e
         * so acontecia depois que o portao abria -- ou seja, no caminho
         * critico da primeira palavra. Somado a deteccao por bloco, era o
         * "atraso bem chato no inicio" que o Celio descreveu.
         *
         * Agora o DAC fica desmutado enquanto a passagem direta esta ativa e o
         * silencio e garantido pelo expansor (-30dB), que e o lugar certo para
         * garanti-lo: ele age no dado, sem escrita de registrador no caminho
         * do som. O rele segue a voz, porque ele tem timeout proprio e ligar o
         * amplificador a toa e outro problema. */
        if (!desmutado_por_nos) {
            audio_codec_set_mute(false);
            desmutado_por_nos = true;
        }
        if (passar) {
            blocos_sem_voz = 0;
            if (!relay_ligado_por_nos) {
                relay_control_notify_playing(true); /* liga o amplificador pra voz sair */
                relay_ligado_por_nos = true;
            }
        } else if (relay_ligado_por_nos) {
            blocos_sem_voz++;
            /* ~172 blocos/s -> ~3s. Tempo suficiente para atravessar as pausas
             * entre frases sem nenhum clique, e curto o bastante para o rele
             * do amplificador ainda cair quando o karaoke acaba. */
            if (blocos_sem_voz > 516) {
                relay_control_notify_playing(false);
                relay_ligado_por_nos = false;
                blocos_sem_voz = 0;
            }
        }

        /* Escrita direta protegida pelo mesmo mutex -- e NAO atualiza
         * s_last_write_us de proposito: senao esta propria escrita pareceria
         * "musica tocando" e a task se desligaria sozinha no ciclo seguinte. */
        if (xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            size_t escritos = 0;
            esp_err_t wr = i2s_channel_write(s_tx_handle, buf, lidos, &escritos, pdMS_TO_TICKS(200));
            xSemaphoreGive(s_i2s_mutex);
            /* So loga FALHA. O log de sucesso a cada 3s existia para provar
             * que a escrita acontecia, na epoca em que o microfone captava e
             * nao saia som -- pergunta ha muito respondida. Ele sozinho enchia
             * o buffer circular de log e atropelava tudo que interessava. */
            if (wr != ESP_OK || escritos < lidos) {
                static int64_t wr_log_us;
                int64_t agora_w = esp_timer_get_time();
                if (agora_w - wr_log_us > 3000000) {
                    wr_log_us = agora_w;
                    logger_log(ESP_LOG_WARN, TAG, "mic: escrita %s, %u de %u bytes",
                               esp_err_to_name(wr), (unsigned)escritos, (unsigned)lidos);
                }
            }
        } else {
            static int64_t mx_log_us;
            int64_t agora_m = esp_timer_get_time();
            if (agora_m - mx_log_us > 3000000) {
                mx_log_us = agora_m;
                logger_log(ESP_LOG_WARN, TAG, "mic: nao consegui o mutex do I2S pra escrever");
            }
        }
    }
}

esp_err_t audio_codec_write(const uint8_t *data, size_t len, size_t *bytes_written)
{
    /* Prazo no mutex e na escrita (eram os dois portMAX_DELAY) -- ver a
     * explicacao completa do impasse em audio_codec_reconfigure_clock().
     * Resumo: uma escrita presa aqui segurava o mutex pra sempre e travava
     * a outra fonte de audio. Um segundo e uma eternidade pra um bloco de
     * PCM (o buffer DMA inteiro tem fracoes de segundo); se estourar, e
     * porque algo esta errado de verdade -- melhor devolver erro, que os
     * chamadores ja tratam, do que congelar o audio ate reiniciar. */
    if (xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "audio_codec_write: mutex do I2S preso por 1s -- descartando este bloco");
        if (bytes_written != NULL) {
            *bytes_written = 0;
        }
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    /* So mistura o microfone se o ADC subiu saudavel -- ver s_mic_adc_estado. */
    if (s_rx_handle != NULL && s_mic_adc_estado == MIC_ADC_OK) {
        err = write_with_mic_mix(data, len, bytes_written);
    } else {
        /* portMAX_DELAY de novo (2026-08-28): o prazo de 1s aqui causava
         * ESCRITA PARCIAL sob qualquer atraso, e quem chama devolve o item do
         * ring buffer inteiro -- o resto do bloco era descartado, picotando o
         * audio (relato ao vivo). Esperar o DMA e o mecanismo natural de
         * ritmo da reproducao. O prazo so fazia sentido enquanto duas fontes
         * disputavam o codec; com o seletor de fonte (audio_source.h) isso
         * nao acontece mais. O prazo no MUTEX (abaixo/acima) fica, como rede
         * de seguranca barata que nao afeta o caminho normal. */
        err = i2s_channel_write(s_tx_handle, data, len, bytes_written, portMAX_DELAY);
    }
    xSemaphoreGive(s_i2s_mutex);
    if (err == ESP_OK && len > 0) {
        s_last_write_us = esp_timer_get_time();
    }
    return err;
}

int64_t audio_codec_last_write_us(void)
{
    return s_last_write_us;
}

esp_err_t audio_codec_reconfigure_clock(uint32_t sample_rate_hz)
{
    /* i2s_channel_reconfig_std_clock() exige o canal desabilitado, senão
     * retorna ESP_ERR_INVALID_STATE e não muda nada — silenciosamente deixa
     * tocando na taxa antiga (distorcido/acelerado) para qualquer faixa que
     * não seja 44100Hz, o default usado em audio_codec_init(). Precisa do
     * mesmo mutex de audio_codec_write(): desabilitar o canal enquanto outra
     * task esta bloqueada dentro de um write() em andamento e a forma mais
     * direta de travar o dispositivo (confirmado na pratica). */
    /* CAUSA RAIZ do "DLNA fica mudo apos interrupcao do BT, so reiniciando
     * resolve" -- diagnosticada 2026-08-27 com instrumentacao passo a passo.
     * O log mostrou a decodificacao FLAC chegando ate o cabecalho pronto
     * (feed r=1) e parando exatamente AQUI, sem nunca logar a linha
     * seguinte.
     *
     * Era um impasse entre tarefas: esta espera era portMAX_DELAY (para
     * sempre) pelo mesmo mutex que audio_codec_write() segura. Quando o BT
     * desconecta com uma escrita I2S em andamento, aquela escrita pode ficar
     * pendurada segurando o mutex -- e a tarefa do DLNA esperava aqui
     * eternamente, com o audio mudo e nada no log. Nem o watchdog resolvia,
     * porque ele agia na conexao HTTP, que estava perfeita.
     *
     * Com prazo: se o mutex nao vier em 2s, desiste e segue tocando na taxa
     * atual (pior caso: uma faixa com taxa diferente sai errada ate a
     * proxima troca) em vez de travar o audio inteiro pra sempre. */
    /* CAUSA RAIZ FINAL (2026-08-27): o prazo acima estourava SEMPRE apos uma
     * sessao de BT (2006ms medidos, contra 5ms no caso normal). Nao era a
     * task do BT segurando o mutex -- e inanicao por prioridade: a task de
     * I2S do DLNA roda em configMAX_PRIORITIES-3 (bem acima desta, que roda
     * em 10) e fica num laco apertado pegando/soltando este mesmo mutex.
     * Sendo mais prioritaria, ela reconquista o mutex toda vez antes que o
     * escalonador devolva a vez pra ca -- e esta tarefa nunca entra.
     *
     * Solucao: sinalizar pra ela ceder enquanto reconfiguramos. Ela checa
     * esta flag antes de tomar o mutex (ver audio_codec_clock_change_pending
     * usado em dlna_renderer.c/bt_audio.c) e dorme, abrindo a janela. */
    s_clock_change_pending = true; /* mantido so como indicador de diagnostico */
    BaseType_t got_mutex = xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(2000));
    s_clock_change_pending = false;
    if (got_mutex != pdTRUE) {
        ESP_LOGE(TAG, "reconfigure_clock: mutex do I2S preso por 2s -- seguindo sem reconfigurar (taxa atual %"
                      PRIu32 "Hz, pedida %" PRIu32 "Hz)",
                 s_current_sample_rate_hz, sample_rate_hz);
        return ESP_ERR_TIMEOUT;
    }
    if (sample_rate_hz == s_current_sample_rate_hz) {
        /* Mesma taxa da faixa anterior (caso comum) -- pula o
         * disable/reenable, que produz um "click" audivel a toa. */
        xSemaphoreGive(s_i2s_mutex);
        return ESP_OK;
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);

    /* O RX (mic) tem que passar pelo MESMO ciclo disable/reconfig/enable que
     * o TX. Em full-duplex no ESP32 o gerador de clock e um so: reconfigurar
     * apenas o TX mudava o BCLK/WS debaixo de um RX que continuava rodando e
     * achando que estava na taxa antiga -- o ADC dessincronizava e a captura
     * virava zeros/lixo assim que a primeira faixa com taxa diferente de
     * 44100Hz comecava. Alem disso o guia do ESP-IDF exige os dois canais no
     * mesmo status (start/stop) durante a operacao. */
    const bool has_rx = (s_rx_handle != NULL);

    esp_err_t err = i2s_channel_disable(s_tx_handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_i2s_mutex);
        return err;
    }
    if (has_rx) {
        i2s_channel_disable(s_rx_handle);
    }

    err = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
    if (err == ESP_OK && has_rx) {
        err = i2s_channel_reconfig_std_clock(s_rx_handle, &clk_cfg);
    }
    if (err != ESP_OK) {
        /* Volta os dois pro ar na taxa antiga -- melhor tocar errado que
         * ficar mudo. */
        i2s_channel_enable(s_tx_handle);
        if (has_rx) {
            i2s_channel_enable(s_rx_handle);
        }
        xSemaphoreGive(s_i2s_mutex);
        return err;
    }

    /* RX ARMADO ANTES DO TX -- mesma correcao de i2s_init(), e aqui ela
     * importa tanto quanto: esta funcao roda a cada troca de faixa com taxa
     * diferente, entao a ordem errada podia DESALINHAR um ADC que ja estava
     * bom no meio do uso. Como o ESP32 e o master, o clock so nasce quando o
     * TX e habilitado; com o RX ja armado, ele engata no primeiro frame em
     * vez de num ponto qualquer. */
    if (has_rx) {
        i2s_channel_enable(s_rx_handle);
    }
    err = i2s_channel_enable(s_tx_handle);
    if (err == ESP_OK) {
        s_current_sample_rate_hz = sample_rate_hz;
    }
    xSemaphoreGive(s_i2s_mutex);
    return err;
}

void audio_codec_play_test_tone(void)
{
    const uint32_t sample_rate = 44100;
    const float freq_hz = 440.0f;
    const float duration_s = 0.6f;
    const float amplitude = 0.8f; /* alto o suficiente pra ser audivel a distancia (ex.: pelo microfone de uma camera de monitoramento) */
    const uint32_t total_samples = (uint32_t)(sample_rate * duration_s);

    relay_control_notify_playing(true);
    /* Amplificadores externos costumam ter um "soft-start"/mudo de
     * protecao contra pop ao ligar (frequentemente 300ms-1s) -- com so
     * 50ms de folga, o bipe inteiro tocava e acabava ANTES do amplificador
     * terminar de "acordar", saindo em silencio mesmo com tudo certo do
     * nosso lado (codec, rele). */
    vTaskDelay(pdMS_TO_TICKS(800));
    audio_codec_set_mute(false);

    int16_t buf[256 * 2]; /* estereo */
    uint32_t done = 0;
    while (done < total_samples) {
        uint32_t chunk = total_samples - done;
        if (chunk > 256) {
            chunk = 256;
        }
        for (uint32_t i = 0; i < chunk; i++) {
            float t = (float)(done + i) / (float)sample_rate;
            int16_t sample = (int16_t)(amplitude * 32767.0f * sinf(2.0f * (float)M_PI * freq_hz * t));
            buf[i * 2] = sample;
            buf[i * 2 + 1] = sample;
        }
        size_t written = 0;
        audio_codec_write((const uint8_t *)buf, chunk * 2 * sizeof(int16_t), &written);
        done += chunk;
    }

    audio_codec_set_mute(true);
    relay_control_notify_playing(false); /* volta ao comportamento normal (desliga apos o timeout configurado) */
}

bool audio_codec_mic_is_enabled(void)
{
    return s_mic_wanted;
}

esp_err_t audio_codec_mic_read(uint8_t *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    if (s_rx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_read(s_rx_handle, data, len, bytes_read, pdMS_TO_TICKS(timeout_ms));
}

/* Reinicia o codec do jeito que SO o reboot conseguia ate agora: matando o
 * MCLK de verdade.
 *
 * Depois de isolar tudo que era acessivel por software (2026-08-30), sobrou
 * uma unica diferenca entre um boot bom e um ruim -- e nao e configuracao. Os
 * 54 registradores do codec sao IDENTICOS nos dois casos. O ruido nao muda com
 * a entrada selecionada (as quatro dao o mesmo nivel), nem com o PGA (de 0 a
 * +21dB nao altera nada), e cai 15x com o volume digital do ADC: ou seja, ele
 * nasce DENTRO do conversor, entre o PGA e o volume digital. E nada recupera:
 * nem reescrever registradores, nem parar e acordar o bloco digital, nem o
 * reset total do chip (0x02=0xFF), nem destruir e recriar os canais I2S, nem
 * reprogramar o clock. So reiniciar o ESP32.
 *
 * O que o reboot faz e nenhuma dessas tentativas fazia: durante o reset o
 * GPIO do MCLK volta a ser entrada, o clock mestre MORRE por um tempo longo e
 * so entao renasce. Destruir os canais deixa o pino solto, nao em nivel baixo,
 * e por pouco tempo -- o codec pode continuar enxergando algo. Aqui o pino e
 * SEGURADO EM ZERO por meio segundo, que e a unica coisa do reboot que faltava
 * reproduzir.
 *
 * Se isso recuperar, deixa de ser um remendo e passa a ser a correcao: o
 * conversor precisa ver o clock morrer para voltar a um estado bom, e o
 * firmware pode dar isso a ele em meio segundo, em vez de reiniciar o aparelho
 * inteiro e fazer o usuario esperar 25 segundos. */
esp_err_t audio_codec_mic_hard_reset(void)
{
    if (s_i2s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    bool tinha_mic = (s_rx_handle != NULL);

    if (s_tx_handle != NULL) {
        i2s_channel_disable(s_tx_handle);
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
    if (s_rx_handle != NULL) {
        i2s_channel_disable(s_rx_handle);
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }

    /* MCLK segurado em ZERO -- nao apenas solto. Um pino em alta impedancia
     * pode continuar entregando algo ao codec por acoplamento; o reset do
     * ESP32 o deixa em entrada com pull-up e o clock realmente cessa. */
    gpio_config_t mclk_off = {
        .pin_bit_mask = 1ULL << PIN_I2S_MCLK,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&mclk_off);
    gpio_set_level(PIN_I2S_MCLK, 0);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Devolve o pino ao periferico I2S e reconstroi tudo, codec inclusive. */
    gpio_reset_pin(PIN_I2S_MCLK);
    esp_err_t err = i2s_init(s_current_sample_rate_hz, false);
    xSemaphoreGive(s_i2s_mutex);
    if (err != ESP_OK) {
        logger_log(ESP_LOG_ERROR, TAG, "mic: hard reset falhou ao recriar o I2S: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50)); /* MCLK estabiliza antes do I2C */
    es8388_init();
    audio_codec_set_volume(audio_codec_get_volume());

    if (tinha_mic) {
        /* Recria o par full-duplex e o ADC pela via normal, que ja tem a
         * ordem correta de enable e a medicao do estado em que subiu. */
        s_mic_adc_estado = MIC_ADC_MEDINDO;
        audio_codec_mic_set_enabled(true);
    }
    logger_log(ESP_LOG_WARN, TAG, "mic: hard reset do codec concluido (MCLK cortado por 500ms)");
    return ESP_OK;
}

esp_err_t audio_codec_mic_set_input(int modo)
{
    esp_err_t err = es8388_mic_set_input_mode((es8388_mic_input_t)modo);
    if (err != ESP_OK) {
        return err;
    }
    storage_set_i32(NVS_KEY_MIC_INPUT, modo);
    logger_log(ESP_LOG_INFO, TAG, "mic: entrada trocada para %s",
               es8388_mic_input_name((es8388_mic_input_t)modo));
    return ESP_OK;
}

int audio_codec_mic_get_input(void)
{
    return (int)es8388_mic_get_input_mode();
}

const char *audio_codec_mic_get_input_name(void)
{
    return es8388_mic_input_name(es8388_mic_get_input_mode());
}

/* Blocos por entrada na varredura. Cada bloco sao 4096 amostras do canal
 * esquerdo, ~93ms a 44,1kHz -- 12 blocos dao ~1,1s de escuta por entrada, o
 * bastante para uma frase cantada, e a varredura inteira fica em ~6s. */
#define MIC_SCAN_AMOSTRAS 4096
#define MIC_SCAN_BLOCOS   12

esp_err_t audio_codec_mic_scan_inputs(char *out, size_t max)
{
    if (out == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rx_handle == NULL) {
        snprintf(out, max, "{\"erro\":\"microfone desligado\"}");
        return ESP_ERR_INVALID_STATE;
    }

    int16_t *buf = heap_caps_malloc(MIC_SCAN_AMOSTRAS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        snprintf(out, max, "{\"erro\":\"sem PSRAM\"}");
        return ESP_ERR_NO_MEM;
    }

    es8388_mic_input_t original = es8388_mic_get_input_mode();
    int melhor = -1;
    int32_t melhor_media = -1;

    size_t pos = 0;
    pos += snprintf(out + pos, max - pos, "{\"entradas\":[");

    for (int m = 0; m < ES8388_IN_COUNT && pos < max; m++) {
        es8388_mic_set_input_mode((es8388_mic_input_t)m);
        /* Descarta o transiente da troca: o PGA e o filtro passa-alta levam
         * um instante para assentar, e o primeiro bloco depois de mexer na
         * entrada traz um degrau que nao representa nada. */
        vTaskDelay(pdMS_TO_TICKS(250));
        audio_codec_mic_capture_raw(buf, MIC_SCAN_AMOSTRAS);

        int32_t pico = 0;
        int64_t soma = 0;
        int64_t n_total = 0;
        for (int b = 0; b < MIC_SCAN_BLOCOS; b++) {
            size_t n = audio_codec_mic_capture_raw(buf, MIC_SCAN_AMOSTRAS);
            for (size_t k = 0; k < n; k++) {
                int32_t a = buf[k] < 0 ? -(int32_t)buf[k] : (int32_t)buf[k];
                if (a > pico) {
                    pico = a;
                }
                soma += a;
            }
            n_total += (int64_t)n;
        }
        int32_t media = n_total > 0 ? (int32_t)(soma / n_total) : 0;
        if (media > melhor_media) {
            melhor_media = media;
            melhor = m;
        }
        pos += snprintf(out + pos, max - pos,
                        "%s{\"modo\":%d,\"nome\":\"%s\",\"media\":%ld,\"pico\":%ld}",
                        m == 0 ? "" : ",", m, es8388_mic_input_name((es8388_mic_input_t)m),
                        (long)media, (long)pico);
    }

    es8388_mic_set_input_mode(original);
    heap_caps_free(buf);

    if (pos < max) {
        pos += snprintf(out + pos, max - pos, "],\"maior\":%d,\"atual\":%d}", melhor, (int)original);
    }
    logger_log(ESP_LOG_INFO, TAG, "mic: varredura de entradas concluida (maior=%s)",
               melhor >= 0 ? es8388_mic_input_name((es8388_mic_input_t)melhor) : "?");
    return ESP_OK;
}

void audio_codec_set_mic_auto_gate(bool enabled)
{
    s_mic_auto_gate = enabled;
    storage_set_i32(NVS_KEY_MIC_AUTO_GATE, enabled ? 1 : 0);
}

bool audio_codec_get_mic_auto_gate(void)
{
    return s_mic_auto_gate;
}
