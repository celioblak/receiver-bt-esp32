#include "slimproto.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "audio_codec.h"
#include "bt_audio.h"
#include "config.h"
#include "flac_stream_decoder.h"
#include "logger.h"
#include "relay_control.h"
#include "storage.h"
#include "wifi_manager.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "slimproto";

#define HEARTBEAT_INTERVAL_US   (5000 * 1000)
#define RECONNECT_DELAY_MS      5000
#define MAX_CTRL_PAYLOAD        768   /* strm (24 + cabecalho HTTP) cabe folgado; qualquer coisa maior e descartada */

/* -------------------------------------------------------------------------
 * Buffer circular + task dedicada de I2S -- mesmo padrao do bt_audio.c.
 * Sem isso, a task de dados escrevia direto no codec logo apos cada
 * recv(), sem nenhuma folga contra instabilidade de rede -- qualquer
 * engasgo/atraso na entrega dos pacotes (confirmado: a mesma instabilidade
 * de WiFi que ja atrapalhava o resto da sessao) virava um engasgo audivel
 * na hora, sem chance de absorver. Com PSRAM sobrando (~4MB livres), um
 * buffer generoso de 64KB (~0.4s de audio 44.1kHz/16-bit estereo) e barato
 * e da folga real. Pre-alocado uma vez em slimproto_init() e fica vivo a
 * vida toda do dispositivo (a task so bloqueia no semaforo quando ocioso,
 * custo de CPU ~zero entre faixas).
 * ------------------------------------------------------------------------- */

/* Aumentado de 64/40KB para 128/80KB (2026-08-12): o gap observado em
 * alguns pulos de faixa (nem todos -- inconsistente, ver
 * project_slimproto_music_assistant memory) bate com a instabilidade de
 * WiFi ja conhecida ([[project_wifi_instability_unresolved]]) -- a troca de
 * faixa abre conexao TCP nova + pede o cabecalho HTTP de novo, exatamente a
 * operacao mais vulneravel a um soluco de rede. Mais folga aqui (dados vem
 * de PSRAM, ~4MB livres -- nao consome a RAM interna escassa que WiFi/BT
 * precisam) da mais margem sem custar nada de RAM interna; so alonga o
 * silencio antes de cada faixa comecar em ~200ms (ainda bem abaixo de
 * perceptivel como "travado"). */
#define SLIM_RINGBUF_HIGHEST_WATER_LEVEL  (128 * 1024)
#define SLIM_RINGBUF_PREFETCH_WATER_LEVEL (80 * 1024)
/* Limite MENOR, so pra RECUPERACAO no meio da faixa (nao pro inicio) --
 * confirmado ao vivo em 2026-08-14 que gaps breves na entrega de rede
 * (so alguns ms sem dado novo, algo normal em WiFi) derrubavam o buffer
 * pro modo PREFETCHING, que ai exigia reencher os 80KB inteiros de novo
 * antes de voltar a tocar -- um soluco de rede de alguns ms virava um
 * engasgo audivel de centenas de ms, varias vezes por faixa (confirmado:
 * 4 ocorrencias em ~30s de uma unica musica). 16KB (~90ms de audio a
 * 44.1kHz/16-bit estereo) e suficiente pra absorver o mesmo tipo de gap
 * que causou o esvaziamento, sem exagerar a resposta. O limite grande
 * (80KB) continua valendo só pra primeira vez que uma faixa comeca (dá
 * folga real contra a abertura de conexao/primeiro pacote), ver
 * s_slim_ringbuf_first_fill. */
#define SLIM_RINGBUF_RECOVERY_WATER_LEVEL (16 * 1024)

typedef enum {
    SLIM_RINGBUF_MODE_PROCESSING,
    SLIM_RINGBUF_MODE_PREFETCHING,
} slim_ringbuf_mode_t;

/* xRingbufferCreate() nao aceita capabilities de memoria -- sempre foi
 * uma aposta implicita que o alocador ia cair pra PSRAM so porque 128KB
 * jamais caberia nos ~178KB de RAM interna do chip inteiro, nunca uma
 * garantia de verdade do codigo (mesmo bug de suposicao do ring buffer do
 * bt_audio.c). Buffer real pedido explicitamente em MALLOC_CAP_SPIRAM via
 * xRingbufferCreateStatic. */
static uint8_t *s_slim_ringbuf_storage = NULL;
static StaticRingbuffer_t s_slim_ringbuf_struct;
static RingbufHandle_t s_slim_ringbuf = NULL;
static SemaphoreHandle_t s_slim_i2s_sem = NULL;
/* Comeca em PREFETCHING (nao PROCESSING): a task escritora fica bloqueada no
 * semaforo ate o buffer atingir o watermark de prefetch pela primeira vez --
 * se comecasse em PROCESSING, nada jamais chamaria xSemaphoreGive() (isso so
 * acontece na transicao PREFETCHING->PROCESSING) e a task dormiria pra
 * sempre. */
static slim_ringbuf_mode_t s_slim_ringbuf_mode = SLIM_RINGBUF_MODE_PREFETCHING;
/* true so na primeira vez que uma faixa enche o buffer (usa o limite grande,
 * 80KB); qualquer reenchimento DEPOIS disso (recuperando de um esvaziamento
 * no meio da musica) usa o limite pequeno, SLIM_RINGBUF_RECOVERY_WATER_LEVEL.
 * Resetado pra true em slim_ringbuf_flush() (inicio de cada faixa nova). */
static bool s_slim_ringbuf_first_fill = true;

/* Quadros (amostras estereo) de verdade escritos no codec desde o inicio da
 * faixa atual -- usado pra calcular elapsed_seconds/elapsed_milliseconds
 * nos STAT que mandamos pro servidor. Confirmado no source real do
 * aioslimproto (client.py, _process_stat_stmt): eles esperam esses campos
 * de verdade pra atualizar a barra de progresso -- mandavamos sempre 0,
 * entao a barra do Music Assistant nunca avançava. So incrementado pela
 * task escritora de I2S (slim_i2s_task_handler), que e quem sabe quanto
 * audio de fato ja saiu pro DAC -- baseado em audio REALMENTE tocado, nao
 * em relogio de parede, entao naturalmente correto mesmo com
 * engasgos/pausas (nada e escrito, o contador nao avança). Zerado no
 * inicio de cada faixa nova (slim_ringbuf_flush()). */
static volatile uint64_t s_slim_frames_played = 0;
static volatile uint32_t s_slim_sample_rate = 44100;

static uint32_t slim_get_elapsed_ms(void)
{
    uint32_t rate = s_slim_sample_rate;
    if (rate == 0) {
        return 0;
    }
    return (uint32_t)((s_slim_frames_played * 1000ULL) / rate);
}

/* Incrementado a cada stop_stream_session() (troca/pulo de faixa OU stop
 * puro). Existe porque fechar o socket de dados NAO interrompe uma task
 * antiga que esteja bloqueada dentro de slim_write_ringbuf() esperando
 * espaco no ring buffer (o backpressure do buffer bloqueia por ate 3s,
 * sem relacao nenhuma com o socket) -- sem isso, um pedaco de audio da
 * faixa ANTERIOR podia "vazar" pro buffer bem depois do flush() da faixa
 * nova ja ter rodado, embaralhando o inicio da faixa nova (o engasgo real
 * ao pular de faixa). Cada slim_write_ringbuf() confere se ainda pertence
 * a sessao atual antes (e durante) a espera; se nao pertence mais,
 * descarta em vez de escrever. */
static volatile uint32_t s_slim_session_id = 0;

/* IMPORTANTE: envio aqui e BLOQUEANTE (nao "tenta e descarta" como no
 * bt_audio.c). Diferenca fundamental do transporte: o A2DP do Bluetooth ja
 * entrega audio no ritmo real de reproducao, entao um buffer cheio la e
 * raro/anormal. Ja o Music Assistant manda o arquivo inteiro via HTTP/TCP o
 * mais rapido que a rede permitir, sem se importar com o ritmo de playback
 * -- descartar pacotes quando o buffer enche (como a primeira versao deste
 * codigo fazia) virava o estado NORMAL de operacao, nao uma excecao, porque
 * a rede sempre entrega mais rapido do que os 176KB/s que o I2S consome.
 * Resultado pratico (confirmado pelo usuario): audio picotado E acelerado,
 * porque trechos inteiros de audio eram jogados fora continuamente.
 * Bloquear aqui aplica backpressure de verdade: a task de dados do
 * slimproto (que chama esta funcao) fica presa, para de chamar recv(), o
 * buffer de recepcao do socket TCP enche, e o proprio TCP freia o servidor
 * automaticamente -- exatamente o mecanismo que a versao anterior (escrita
 * direta e bloqueante no codec) tinha de graca e que a versao "buffered com
 * descarte" tinha quebrado. */
static size_t slim_write_ringbuf(uint32_t session_id, const uint8_t *data, size_t size)
{
    if (s_slim_ringbuf == NULL || size == 0) {
        return 0;
    }

    /* Espera em fatias curtas (nao um unico xRingbufferSend de 3s) e confere
     * a sessao a cada volta -- se a faixa foi trocada/parada enquanto esta
     * chamada estava bloqueada esperando espaco, aborta e descarta em vez de
     * escrever um pedaco atrasado da faixa ANTERIOR depois que a faixa nova
     * ja comecou (era exatamente isso que causava o engasgo ao pular de
     * faixa mesmo com o flush() no inicio da task nova: fechar o socket nao
     * interrompe quem ja estava bloqueado aqui dentro). */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    for (;;) {
        if (session_id != s_slim_session_id) {
            logger_log(ESP_LOG_WARN, TAG, "slim_write_ringbuf: descartando (sessao %u, atual %u)",
                       (unsigned)session_id, (unsigned)s_slim_session_id);
            return 0; /* sessao cancelada -- descarta silenciosamente */
        }
        BaseType_t done = xRingbufferSend(s_slim_ringbuf, (void *)data, size, pdMS_TO_TICKS(100));
        if (done) {
            break;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            logger_log(ESP_LOG_ERROR, TAG, "ring buffer de audio slimproto nao drenou em 3s (sessao %u), descartando pacote",
                       (unsigned)session_id);
            return 0;
        }
        /* ainda dentro do prazo total -- tenta de novo (proxima volta confere a sessao) */
    }

    if (s_slim_ringbuf_mode == SLIM_RINGBUF_MODE_PREFETCHING) {
        /* REVERTIDO (2026-08-14): o limite pequeno de recuperacao (16KB)
         * pareceu uma boa ideia (gap curto = reenchimento rapido), mas ao
         * testar ao vivo o engasgo ficou MUITO mais frequente (varias vezes
         * por segundo em vez de a cada 5-9s) e o usuario confirmou de
         * ouvido que ficou pior, nao melhor. Hipotese corrigida: o problema
         * nao e um gap curto isolado -- e um trecho sustentado onde a
         * chegada de dados (rede) mal acompanha o consumo (I2S), entao um
         * buffer pequeno de recuperacao esvazia de novo quase imediatamente,
         * virando um ciclo de esvaziar->reencher continuo (pior audivelmente
         * que os poucos engasgos maiores, mas mais raros, do buffer grande).
         * De volta ao limite unico de 80KB pros dois casos -- fica como
         * pendente pra investigar de outro angulo (ex.: velocidade real do
         * decode FLAC vs necessidade de I2S) numa sessao com mais folego. */
        size_t used = 0;
        vRingbufferGetInfo(s_slim_ringbuf, NULL, NULL, NULL, NULL, &used);
        if (used >= SLIM_RINGBUF_PREFETCH_WATER_LEVEL) {
            s_slim_ringbuf_mode = SLIM_RINGBUF_MODE_PROCESSING;
            xSemaphoreGive(s_slim_i2s_sem);
        }
    }
    return size;
}

/* Chamada no inicio de cada nova faixa (slimproto_data_task), antes de
 * reconfigurar o clock do I2S ou empurrar qualquer byte da faixa nova.
 * Sem isso, sobras de audio da faixa ANTERIOR (ate ~360ms, o tamanho do
 * buffer) ainda paradas no ring buffer viravam vitima do
 * audio_codec_reconfigure_clock() da faixa nova (desabilita/reabilita o
 * canal I2S no meio do caminho) -- e exatamente o cenario de troca/pulo de
 * faixa que causava o engasgo relatado mesmo depois do buffer resolver o
 * picotamento durante uma musica so. */
static void slim_ringbuf_flush(void)
{
    if (s_slim_ringbuf == NULL) {
        return;
    }
    for (;;) {
        size_t item_size = 0;
        void *data = xRingbufferReceiveUpTo(s_slim_ringbuf, &item_size, 0, SLIM_RINGBUF_HIGHEST_WATER_LEVEL);
        if (item_size == 0) {
            break;
        }
        vRingbufferReturnItem(s_slim_ringbuf, data);
    }
    s_slim_ringbuf_mode = SLIM_RINGBUF_MODE_PREFETCHING;
    s_slim_ringbuf_first_fill = true;
    s_slim_frames_played = 0;
}

static void slim_i2s_task_handler(void *arg)
{
    const size_t max_chunk = 240 * 6;

    for (;;) {
        if (xSemaphoreTake(s_slim_i2s_sem, portMAX_DELAY) == pdTRUE) {
            for (;;) {
                size_t item_size = 0;
                uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
                    s_slim_ringbuf, &item_size, pdMS_TO_TICKS(20), max_chunk);
                if (item_size == 0) {
                    /* Diagnostico temporario (2026-08-14): engasgos audiveis
                     * relatados sem NENHUM erro correspondente em nenhum log
                     * ate agora -- suspeita e que isso aqui e o mecanismo
                     * real: um gap de so 20ms sem dado novo no ring buffer
                     * ja derruba pro modo PREFETCHING de novo, que exige
                     * reencher ate 80KB (SLIM_RINGBUF_PREFETCH_WATER_LEVEL)
                     * antes de tocar mais alguma coisa -- um soluco breve
                     * vira um engasgo bem mais longo, sem logar nada como
                     * "erro" hoje. */
                    logger_log(ESP_LOG_WARN, TAG,
                               "slim_i2s: buffer esvaziou (20ms sem dado) -- voltando pra PREFETCHING (reenche ate %uKB)",
                               (unsigned)(SLIM_RINGBUF_PREFETCH_WATER_LEVEL / 1024));
                    s_slim_ringbuf_mode = SLIM_RINGBUF_MODE_PREFETCHING;
                    break;
                }
                /* Diagnostico temporario (2026-08-14): log periodico do nivel
                 * do buffer, pra saber se o esvaziamento e uma queda SUBITA
                 * (rajada breve de contencao de CPU/rede) ou um DECLINIO
                 * GRADUAL (decode+rede simplesmente nao sustentam o ritmo
                 * medio de consumo do I2S por tempo suficiente). */
                static int64_t s_last_fill_log_us = 0;
                int64_t now_us = esp_timer_get_time();
                if (now_us - s_last_fill_log_us > 500000) {
                    size_t used = 0;
                    vRingbufferGetInfo(s_slim_ringbuf, NULL, NULL, NULL, NULL, &used);
                    logger_log(ESP_LOG_INFO, TAG, "slim_i2s: nivel do buffer: %uKB", (unsigned)(used / 1024));
                    s_last_fill_log_us = now_us;
                }
                size_t written = 0;
                audio_codec_write(data, item_size, &written);
                s_slim_frames_played += written / 4; /* 4 bytes/quadro (16-bit estereo) */
                vRingbufferReturnItem(s_slim_ringbuf, data);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Estado (consultado pela API REST — ver web_server.c)
 * ------------------------------------------------------------------------- */

static SemaphoreHandle_t s_status_mutex;
static slimproto_status_t s_status = {0};

/* IP (network order) do servidor de controle -- usado como fallback quando
 * o STRM recebido traz server_ip=0 ("use o mesmo servidor da conexao de
 * controle"), o caso mais comum na pratica. */
static uint32_t s_control_server_ip;

/* Socket da conexao de DADOS (separada da de controle) da sessao atual --
 * protegido por mutex porque a task de controle (ao receber um novo 's' ou
 * um 'q'/'f') pode fechar esse socket enquanto a task de dados ainda esta
 * bloqueada num recv() nele; e assim que sinalizamos "para" pra ela, sem
 * precisar (nem arriscar) chamar vTaskDelete de fora. */
static SemaphoreHandle_t s_data_mutex;
static int s_data_sock = -1;
static volatile bool s_data_paused = false;

/* Instante (esp_timer_get_time(), us) da ultima transicao de sessao
 * (stop_stream_session -- toda troca de faixa passa por ali, natural ou
 * via comando). Exposto pra lms_cli.c poder dar uma folga antes de abrir
 * sua propria conexao TCP nova (porta 9090) se uma transicao acabou de
 * acontecer -- ver slimproto_get_last_transition_time_us() e o comentario
 * em lms_cli.c. Reduz (nao elimina) a chance de duas operacoes de socket
 * concorrentes (nossa e a do Slimproto) disputarem as mesmas estruturas
 * internas do lwIP (tcpip_thread) ao mesmo tempo -- causa suspeita de
 * pelo menos 3 crashes reais distintos vistos ao vivo em 2026-08-16. */
static volatile int64_t s_last_session_transition_us = 0;

/* Sinaliza quando a task de dados (slim_data) de fato terminou e chamou
 * vTaskDelete -- ver start_stream_session() (onde e consumido) e o fim de
 * slimproto_data_task() (onde e dado). Substitui um vTaskDelay(50) fixo que
 * nao tinha garantia nenhuma contra o caso de pulo rapido de faixa (varios
 * strm em poucos ms, ex.: next/previous via lms_cli): se a task antiga
 * ainda estivesse rodando (presa numa etapa lenta do decode, ou atrasada
 * por disputa de CPU/radio com o proprio comando que disparou o pulo) e o
 * static stack fosse reusado por uma task nova antes dela realmente sair,
 * as duas ficariam escrevendo no MESMO buffer de stack ao mesmo tempo --
 * corrupcao de memoria real, PANIC. Um semaforo binario aguardado com
 * timeout elimina a adivinhacao: espera exatamente ate a task antiga
 * confirmar que saiu (ou ate o timeout, se nao havia task antiga nenhuma,
 * ex. primeira faixa apos o boot). */
static SemaphoreHandle_t s_data_task_exited_sem;

/* Stack da task de dados (slim_data) ESTATICO, nao dinamico -- confirmado ao
 * vivo (heap_caps_get_largest_free_block) que o maior bloco CONTIGUO de RAM
 * interna disponivel em runtime, mesmo saudavel, fica travado em ~8192
 * bytes -- pedir exatamente esse tanto pro xTaskCreate ainda falha (o TCB
 * do FreeRTOS precisa de um pouco mais que o stack puro). Um buffer static
 * e reservado pelo LINKER, em tempo de compilacao -- nao compete por um
 * bloco contiguo em runtime, entao nao tem esse jeito de falhar. Reutilizado
 * a cada faixa (xTaskCreateStatic de novo, mesmo buffer) depois que a task
 * anterior se autodeleta -- a folga de 50ms antes de criar (ver
 * start_stream_session) ja garante que o idle task processou a delecao
 * anterior antes de reusarmos o mesmo TCB/stack. */
/* De volta a 8192 (2026-08-14): tentamos 6144 mais cedo hoje (telemetria de
 * high-water-mark sugeria so ~3824 bytes de pico, entao 6144 parecia ter
 * folga de sobra) -- trouxe RAM interna real (mdns_send parou de falhar,
 * WS do MA autenticou de primeira), MAS o dispositivo travou bem na hora
 * de um play real logo depois. As duas medicoes de high-water-mark que
 * embasaram o 6144 podem nao ter cobrido o pior caso de verdade da cadeia
 * de decode C++ do FLAC (profundidade varia com bloco/canal/profundidade
 * de bits) -- o PANIC original que comecou toda essa investigacao era
 * exatamente isso.
 *
 * Subiu pra 16384 (2026-08-16): 8192 tambem nao era seguro o suficiente --
 * dois PANICs reais (reset reason confirmado em /api/logs) aconteceram no
 * mesmo dia, em testes separados, ambos SEM nenhuma mudanca de codigo nova
 * envolvida (rodando o mesmo binario que ja tinha ficado 11h+ estavel antes
 * disso -- ou seja, e intermitente/dependente do conteudo real da faixa,
 * nao de uma regressao pontual). Subiu pra 16384 (valor de producao do
 * squeezelite-esp32, `DECODE_THREAD_STACK_SIZE` em `embedded.h`) por
 * algumas horas, mas **voltou pra 8192 no mesmo dia** depois de confirmar
 * ao vivo, com captura serial de verdade (nao mais suposicao), que o custo
 * era real e imediato: `wifi:mem fail` sustentado, MQTT caindo por timeout
 * de conexao, RAM interna medida em ~8.6KB livre. E a telemetria de
 * high-water-mark da PROPRIA sessao que rodou nesse teste (ver
 * `uxTaskGetStackHighWaterMark` no fim desta task) mostrou uso real de
 * pilha de so ~3.7KB (12656 bytes livres de 16384) -- ou seja, nem chegou
 * perto de precisar dos 16KB inteiros. Motivo mais provavel dos PANICs
 * originais: nao estouro de pilha por decode profundo, e sim a CORRIDA
 * entre a task antiga (ainda saindo) e a nova (reusando o mesmo static
 * stack/TCB) durante pulos rapidos de faixa -- ver `s_data_task_exited_sem`
 * acima, que resolve isso de verdade (sincronizacao real, nao um
 * vTaskDelay(50) as cegas). Se um PANIC voltar a acontecer mesmo com o
 * semaforo em vigor E 8192, aí sim teriamos evidencia real de que
 * profundidade de pilha e o problema, e valeria subir nesse caso -- mas
 * com dados de uma falha real, nao de novo por suposicao. NAO mover esse
 * stack pra PSRAM como alternativa: essa task faz rede (sockets lwIP), e
 * PSRAM fica inacessivel durante qualquer escrita concorrente na
 * flash/NVS por outra task (limitacao real de cache do ESP32) -- um stack
 * de execucao em PSRAM travaria de um jeito pior e mais imprevisivel. */
#define SLIM_DATA_STACK_BYTES 8192
static StackType_t s_slim_data_stack[SLIM_DATA_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_slim_data_tcb;

/* Socket da conexao de CONTROLE (a mesma que slimproto_control_task usa
 * pros proprios STMt periodicos) -- exposto aqui pra a task de DADOS
 * conseguir mandar um STAT quando a faixa termina naturalmente (EOF limpo
 * do socket de dados), sem precisar de outra conexao. Protegido por mutex
 * proprio: sem isso, duas tasks diferentes escrevendo na mesma conexao TCP
 * ao mesmo tempo (o heartbeat periodico da propria control task vs esse
 * aviso vindo da data task) arrisca uma escrita entrelacada/corrompida. */
static SemaphoreHandle_t s_ctrl_mutex;
static int s_ctrl_sock = -1;

typedef struct {
    uint32_t server_ip;   /* network order */
    uint16_t server_port;
    char http_header[MAX_CTRL_PAYLOAD];
    size_t http_header_len;
    char format; /* 'p' = PCM/WAV, 'f' = FLAC -- ver handle_strm()/slimproto_data_task() */
    char pcm_sample_size;
    char pcm_sample_rate;
    char pcm_channels;
    char pcm_endian;
    uint32_t session_id;
} stream_start_args_t;

static void slimproto_data_task(void *arg);

/* -------------------------------------------------------------------------
 * Codificação/decodificação de campos binários (big-endian, ver protocolo
 * Slimproto/LMS: https://wiki.lyrion.org/index.php/SlimProto_TCP_protocol.html)
 * ------------------------------------------------------------------------- */

static void put_u8(uint8_t **p, uint8_t v) { *(*p)++ = v; }

static void put_u16(uint8_t **p, uint16_t v)
{
    uint16_t be = htons(v);
    memcpy(*p, &be, 2);
    *p += 2;
}

static void put_u32(uint8_t **p, uint32_t v)
{
    uint32_t be = htonl(v);
    memcpy(*p, &be, 4);
    *p += 4;
}

static void put_u64(uint8_t **p, uint64_t v)
{
    put_u32(p, (uint32_t)(v >> 32));
    put_u32(p, (uint32_t)(v & 0xffffffffu));
}

static uint16_t get_u16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return ntohs(v);
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return ntohl(v);
}

/* -------------------------------------------------------------------------
 * Envio de mensagens (opcode ASCII de 4 bytes + tamanho + payload)
 * ------------------------------------------------------------------------- */

static bool send_all(int sock, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool send_message(int sock, const char *opcode4, const uint8_t *payload, size_t payload_len)
{
    uint8_t header[8];
    memcpy(header, opcode4, 4);
    uint32_t len_be = htonl((uint32_t)payload_len);
    memcpy(header + 4, &len_be, 4);
    if (!send_all(sock, header, sizeof(header))) {
        return false;
    }
    return payload_len == 0 || send_all(sock, payload, payload_len);
}

static bool send_helo(int sock)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    /* So anunciamos "pcm" -- forca o servidor a sempre mandar PCM cru (sem
     * FLAC/MP3/etc), que e o que audio_codec_write() consome direto, sem
     * decoder nenhum no ESP32.
     *
     * Campos (Model=squeezelite, AccuratePlayPoints=1, HasDigitalOut=1,
     * HasPolarityInversion=1, Firmware=) copiados EXATAMENTE do BASE_CAP do
     * squeezelite-esp32 real (que funciona com o MA), em vez dos valores
     * "genericos" que tinhamos antes -- suspeita (nao 100% confirmada, o
     * codigo do provider do MA que decide isso nao ficou totalmente claro
     * nem investigando o source) de que o MA trata "Model=squeezelite" como
     * cliente conhecido/confiavel pra negociacao de codec, e um Model
     * desconhecido cai num fallback mais conservador (FLAC) ignorando o
     * "pcm" que a gente ja mandava certo na lista de codecs. */
    /* "pcm,flc": pcm primeiro (preferencia), flc so pra nao disparar o aviso
     * "Player did not report support for content_type audio/flac" do
     * aioslimproto -- confirmado via log de debug do MA que esse aviso e
     * seguido de silencio total (nenhum STRM chega no dispositivo depois
     * dele). NAO decodificamos FLAC de verdade; a esperanca e que o MA
     * escolha PCM por estar primeiro na lista quando os dois batem. Se o
     * servidor mandar formato 'f' mesmo assim, handle_strm() ja rejeita e
     * loga, sem travar nada -- so nao vai tocar essa faixa especificamente. */
    static const char capabilities[] =
        "Model=squeezelite,AccuratePlayPoints=1,HasDigitalOut=1,"
        "HasPolarityInversion=1,Firmware=" FW_VERSION
        ",ModelName=ReceiverBT,MaxSampleRate=48000,pcm,flc";

    uint8_t payload[1 + 1 + 6 + 16 + 2 + 8 + 2 + sizeof(capabilities)];
    uint8_t *p = payload;
    put_u8(&p, 12); /* deviceid: "squeezeplay" -- generico, cliente de software */
    put_u8(&p, 0);  /* revision */
    memcpy(p, mac, 6);
    p += 6;
    memset(p, 0, 16); /* uuid -- nao usado */
    p += 16;
    put_u16(&p, 0); /* wlan_channellist */
    put_u64(&p, 0); /* bytes_received */
    memcpy(p, "en", 2);
    p += 2;
    size_t cap_len = strlen(capabilities);
    memcpy(p, capabilities, cap_len);
    p += cap_len;

    return send_message(sock, "HELO", payload, (size_t)(p - payload));
}

/* server_timestamp: só é relevante em resposta a um 'strm t' (ecoa de volta
 * o campo replay_gain recebido) -- 0 nos demais eventos. Os campos de
 * buffer/posição são só informativos pro servidor/UI; não rastreamos com
 * precisão. elapsed_seconds/elapsed_milliseconds ANTES eram sempre 0 --
 * confirmado no source real do aioslimproto (client.py,
 * _process_stat_stmt) que eles usam esses dois campos pra atualizar a
 * barra de progresso da faixa no Music Assistant; mandar sempre 0 e o
 * motivo dela nunca avançar. Agora usa slim_get_elapsed_ms() (contador
 * real de quadros escritos no codec desde o inicio da faixa, ver
 * s_slim_frames_played). */
static bool send_stat(int sock, const char *event_code, uint32_t server_timestamp)
{
    uint32_t elapsed_ms = slim_get_elapsed_ms();

    uint8_t payload[53];
    uint8_t *p = payload;
    memcpy(p, event_code, 4);
    p += 4;
    put_u8(&p, 0); /* crlf count */
    put_u8(&p, 0); /* mas_initialized */
    put_u8(&p, 0); /* mas_mode */
    put_u32(&p, 8192); /* stream_buffer_size (informativo) */
    put_u32(&p, 0);    /* stream_buffer_fullness */
    put_u64(&p, 0);    /* bytes_received */
    put_u16(&p, 0xffff); /* signal_strength: N/A (equivalente a "com fio") */
    put_u32(&p, (uint32_t)(esp_timer_get_time() / 1000)); /* jiffies (ms desde boot) */
    put_u32(&p, 8192); /* output_buffer_size */
    put_u32(&p, 0);    /* output_buffer_fullness */
    put_u32(&p, elapsed_ms / 1000); /* elapsed_seconds */
    put_u16(&p, 0);    /* voltage */
    put_u32(&p, elapsed_ms); /* elapsed_milliseconds */
    put_u32(&p, server_timestamp);
    put_u16(&p, 0); /* error_code */

    return send_message(sock, "STAT", payload, (size_t)(p - payload));
}

/* Versao thread-safe de send_stat() usando o socket de controle compartilhado
 * (s_ctrl_sock) -- pra qualquer chamada que NAO seja de dentro da propria
 * slimproto_control_task (que ja tem acesso direto e exclusivo ao seu
 * socket local). Usado pela task de DADOS pra avisar o servidor quando uma
 * faixa termina naturalmente (ver handle_track_ended_naturally()). */
static bool send_stat_safe(const char *event_code, uint32_t server_timestamp)
{
    xSemaphoreTake(s_ctrl_mutex, portMAX_DELAY);
    int sock = s_ctrl_sock;
    bool ok = (sock >= 0) && send_stat(sock, event_code, server_timestamp);
    xSemaphoreGive(s_ctrl_mutex);
    return ok;
}

/* -------------------------------------------------------------------------
 * Sessão de dados (conexão HTTP separada, aberta a pedido de um 'strm s')
 * ------------------------------------------------------------------------- */

static void stop_stream_session(void)
{
    uint32_t old_id = s_slim_session_id;
    s_slim_session_id++; /* invalida qualquer write bloqueado da sessao anterior -- ver slim_write_ringbuf() */
    logger_log(ESP_LOG_INFO, TAG, "stop_stream_session: sessao %u -> %u (sock=%d)",
               (unsigned)old_id, (unsigned)s_slim_session_id, s_data_sock);
    /* Marca o instante desta transicao -- ver slimproto_get_last_transition_time_us()
     * e o comentario em lms_cli.c sobre por que isso importa. */
    s_last_session_transition_us = esp_timer_get_time();
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (s_data_sock >= 0) {
        shutdown(s_data_sock, SHUT_RDWR);
        close(s_data_sock);
        s_data_sock = -1;
    }
    xSemaphoreGive(s_data_mutex);
    /* Não chamamos vTaskDelete aqui de propósito -- a task de dados percebe
     * o socket fechado (recv retorna erro) e se autodeleta sozinha. Isso
     * evita qualquer corrida de double-delete entre "quem manda parar" e
     * "quem termina por conta própria" (fim normal do stream). */
}

static const int PCM_RATE_TABLE[10] = {
    11025, 22050, 32000, 44100, 48000, 8000, 12000, 16000, 24000, 96000,
};

static int pcm_rate_to_hz(char c)
{
    if (c >= '0' && c <= '9') {
        return PCM_RATE_TABLE[c - '0'];
    }
    return 44100; /* '?' = auto-descritivo (cabeçalho WAV) -- não suportado ainda, usa um default razoável */
}

/* '0'=8-bit, '1'=16-bit, '2'=20/24-bit (empacotado em 3 bytes na pratica),
 * '3'=32-bit. Precisamos disso porque o Music Assistant manda WAV em
 * 24-bit por padrao (so descobrimos tocando de verdade -- deu um chiado
 * constante, amostras de 16-bit sendo lidas erradas a partir de dado de
 * 24-bit). audio_codec_write() so aceita 16-bit, entao convertemos aqui. */
static size_t pcm_bytes_per_sample(char c)
{
    switch (c) {
        case '0': return 1;
        case '2': return 3;
        case '3': return 4;
        default: return 2; /* '1' e qualquer coisa inesperada -- 16-bit e o caso comum */
    }
}

/* Reduz uma amostra de N bytes pra 16 bits pegando os bytes mais
 * significativos (truncamento simples, sem dither -- suficiente aqui,
 * a perda de precisao e inaudivel nesse uso). */
static int16_t pcm_extract_16bit(const uint8_t *p, size_t bytes_per_sample, bool big_endian)
{
    if (bytes_per_sample == 1) {
        /* 8-bit PCM e convencionalmente sem sinal (0-255, meio-de-escala=128) */
        return (int16_t)(((int)p[0] - 128) << 8);
    }
    if (big_endian) {
        return (int16_t)((p[0] << 8) | p[1]);
    }
    /* little-endian: os bytes mais significativos ficam no final da amostra */
    return (int16_t)((p[bytes_per_sample - 1] << 8) | p[bytes_per_sample - 2]);
}

/* Comparacao de substring sem diferenciar maiusculas/minusculas -- so pra
 * achar "chunked" dentro do texto cru dos headers HTTP. */
static bool contains_ci(const char *haystack, const char *needle)
{
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn == 0 || nn > hn) {
        return false;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for (; j < nn; j++) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) {
                break;
            }
        }
        if (j == nn) {
            return true;
        }
    }
    return false;
}

/* O corpo da resposta HTTP pode vir com "Transfer-Encoding: chunked"
 * (esperado pra um stream gerado ao vivo, de tamanho desconhecido -- e
 * exatamente o caso do Music Assistant transcodificando em tempo real).
 * Sem desembrulhar isso, os marcadores de tamanho de cada chunk (texto
 * ASCII em hexadecimal + CRLF, intercalados com os dados de verdade) eram
 * lidos como se fossem amostras de audio -- causa real de um chiado
 * constante E do audio ficando mais lento (amostras falsas empurrando o
 * conteudo real pra "depois" no tempo). */
typedef struct {
    bool chunked;
    uint32_t chunk_remaining; /* bytes que ainda faltam do chunk atual (so dados, sem contar o CRLF final) */
    bool need_chunk_size;     /* true = a proxima coisa a ler e uma linha "tamanho\r\n" */
} http_body_reader_t;

/* Le ate max_len bytes de conteudo JA desembrulhado (se chunked). Mesma
 * convencao de retorno do recv(): >0 bytes lidos, 0 = fim do corpo
 * (ultimo chunk "0\r\n" visto), <0 = erro (com errno setado pelo recv()
 * interno). */
static int http_body_read(int sock, http_body_reader_t *r, uint8_t *buf, size_t max_len)
{
    if (!r->chunked) {
        return recv(sock, buf, max_len, 0);
    }
    for (;;) {
        if (r->need_chunk_size) {
            char line[16];
            size_t len = 0;
            for (;;) {
                char c;
                int n = recv(sock, &c, 1, 0);
                if (n <= 0) {
                    return n;
                }
                if (c == '\n') {
                    break;
                }
                if (c != '\r' && len < sizeof(line) - 1) {
                    line[len++] = c;
                }
            }
            line[len] = '\0';
            for (size_t i = 0; i < len; i++) {
                if (line[i] == ';') { /* extensao de chunk (raro) -- ignora */
                    line[i] = '\0';
                    break;
                }
            }
            uint32_t size = (uint32_t)strtoul(line, NULL, 16);
            if (size == 0) {
                return 0; /* chunk final -- fim do corpo */
            }
            r->chunk_remaining = size;
            r->need_chunk_size = false;
        }
        if (r->chunk_remaining == 0) {
            /* acabou o chunk anterior -- engole o CRLF final dele antes do proximo tamanho */
            char crlf[2];
            int n = recv(sock, crlf, sizeof(crlf), 0);
            if (n <= 0) {
                return n;
            }
            r->need_chunk_size = true;
            continue;
        }
        size_t want = max_len < r->chunk_remaining ? max_len : r->chunk_remaining;
        int n = recv(sock, buf, want, 0);
        if (n <= 0) {
            return n;
        }
        r->chunk_remaining -= (uint32_t)n;
        return n;
    }
}

/* Le exatamente len bytes (via http_body_read, entao ja desembrulhado se
 * chunked), bloqueando -- usado so pra cabecalhos pequenos (WAV), onde um
 * n<=0 qualquer e tratado como falha; nao acontece na pratica porque
 * cabecalho chega inteiro quase instantaneo. */
static bool body_read_n(int sock, http_body_reader_t *r, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = http_body_read(sock, r, buf + got, len - got);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static void start_stream_session(uint32_t server_ip, uint16_t server_port,
                                  const uint8_t *header, size_t header_len, char format,
                                  char pcm_size, char pcm_rate, char pcm_channels, char pcm_endian)
{
    stop_stream_session();

    stream_start_args_t *args = malloc(sizeof(*args));
    if (args == NULL) {
        ESP_LOGE(TAG, "sem heap para iniciar sessao de audio");
        return;
    }
    args->server_ip = server_ip != 0 ? server_ip : s_control_server_ip;
    args->server_port = server_port != 0 ? server_port : SLIMPROTO_SERVER_PORT;
    size_t copy_len = header_len < sizeof(args->http_header) ? header_len : sizeof(args->http_header);
    memcpy(args->http_header, header, copy_len);
    args->http_header_len = copy_len;
    args->format = format;
    args->pcm_sample_size = pcm_size;
    args->pcm_sample_rate = pcm_rate;
    args->pcm_channels = pcm_channels;
    args->pcm_endian = pcm_endian;
    args->session_id = s_slim_session_id; /* stop_stream_session() acima ja incrementou */

    if (pcm_size == '?' || pcm_rate == '?' || pcm_channels == '?') {
        /* "Auto-descritivo": o header STRM nao traz o formato real, so o
         * WAV que vem no corpo (RIFF/fmt). Isso ja e coberto -- o parser de
         * WAV em slimproto_data_task le o chunk 'fmt ' de verdade e
         * substitui taxa/canais/bits antes de qualquer conversao (ver o
         * bloco "fmt real do WAV" mais abaixo). O valor guardado aqui
         * (args->pcm_sample_size etc.) e so um palpite de partida, sem
         * efeito pratico no audio final. */
        logger_log(ESP_LOG_INFO, TAG, "PCM auto-descritivo (WAV) recebido -- formato real vira do cabecalho WAV no corpo");
    }

    s_data_paused = false;

    /* Espera real (nao mais um vTaskDelay(50) fixo, ver comentario em
     * s_data_task_exited_sem) ate a task de dados da sessao anterior (se
     * houver) confirmar que saiu de vez -- so entao e seguro reusar o
     * static stack/TCB pra uma task nova. Timeout de 200ms e so uma rede de
     * seguranca (primeira faixa apos o boot, sem task anterior nenhuma
     * pendente) -- no caso comum, se a task antiga ja tinha saido antes
     * desta chamada, o take() retorna na hora, sem nenhuma espera. */
    xSemaphoreTake(s_data_task_exited_sem, pdMS_TO_TICKS(200));

    /* CRITICO, achado via backtrace real de um crash ao vivo (2026-08-16):
     * o semaforo acima so garante que o CODIGO da task antiga terminou e
     * chamou vTaskDelete() -- mas vTaskDelete() so MARCA a task pra
     * remocao, quem de fato tira ela das listas internas do kernel
     * (ready list etc.) e a task IDLE, rodando depois, de forma
     * assincrona. Sem essa folga, xTaskCreateStatic() logo abaixo podia
     * reusar o mesmo TCB/stack ANTES da IDLE ter processado a remocao da
     * task antiga -- crash confirmado com backtrace real dentro do
     * proprio FreeRTOS (prvAddNewTaskToReadyList, chamado por
     * xTaskCreateStatic aqui). Mesma licao ja aprendida neste projeto
     * sobre taskYIELD()/vTaskDelay(0) NAO bastarem pra deixar a IDLE
     * rodar -- precisa de um vTaskDelay(>=1) de verdade. */
    vTaskDelay(pdMS_TO_TICKS(1));

    /* Stack ESTATICO (s_slim_data_stack, declarado no topo do arquivo), nao
     * xTaskCreate dinamico -- ja tentamos 16KB (persistente, afogou
     * MQTT/WiFi no boot), depois 10KB e 8KB por faixa (heap_caps_
     * get_largest_free_block() mostrou o maior bloco CONTIGUO de RAM
     * interna travado em ~8192 bytes mesmo com o sistema saudavel, e pedir
     * exatamente esse valor ainda falhava por causa do overhead do TCB).
     * xTaskCreateStatic() nao sofre disso: o espaco e reservado pelo
     * LINKER, nao competido em runtime, entao sempre da certo. */
    /* SEGUNDA TENTATIVA (2026-08-14) de fixar essa task no APP_CPU (core 1)
     * -- REVERTIDA DE NOVO no mesmo dia: mesmo com o vTaskDelay(1) real a
     * cada 32 voltas do loop de decode (ver comentario la), o dispositivo
     * travou por completo (rede/HTTP inteiramente sem resposta, precisou
     * de reset fisico via esptool) poucos minutos depois de gravar essa
     * versao -- pior que os engasgos que essa mudanca tentava resolver, e
     * sem confirmacao de que o pin estivesse realmente ajudando antes do
     * travamento. De volta a xTaskCreateStatic sem pin (deixa o scheduler
     * distribuir entre os dois nucleos). O vTaskDelay(1) periodico no loop
     * de decode foi MANTIDO (nao tem custo/risco conhecido sozinho, sem
     * pin) -- se o pin for tentado uma terceira vez no futuro, investigar
     * a fundo a causa do travamento antes (pode nao ter sido so o watchdog
     * de interrupcao; capturar serial ao vivo seria necessario). */
    TaskHandle_t data_task = xTaskCreateStatic(slimproto_data_task, "slim_data",
                                                sizeof(s_slim_data_stack), args, 5,
                                                s_slim_data_stack, &s_slim_data_tcb);
    if (data_task == NULL) {
        logger_log(ESP_LOG_ERROR, TAG, "falha ao criar task de dados (inesperado -- stack e static)");
        free(args);
    }
}

static void slimproto_data_task(void *arg)
{
    stream_start_args_t *args = (stream_start_args_t *)arg;
    uint32_t my_session = args->session_id;

    logger_log(ESP_LOG_INFO, TAG, "slimproto_data_task: iniciando sessao %u", (unsigned)my_session);
    slim_ringbuf_flush(); /* descarta sobra da faixa anterior antes de tocar a nova */

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(args->server_port);
    addr.sin_addr.s_addr = args->server_ip;

    /* socket() as vezes falha ("lwip_arch: thread_sem_init: out of memory")
     * bem no meio de uma rajada de comandos strm (f/q/s em sequencia rapida,
     * cada um fechando a sessao anterior e abrindo uma nova em milissegundos)
     * mesmo com RAM interna geral saudavel (~23KB livres, confirmado ao vivo
     * em 2026-08-14) -- parece ser contencao transitoria (cleanup assincrono
     * de sockets anteriores ainda em andamento), nao falta de memoria de
     * verdade. Retry curto em vez de desistir na primeira falha. */
    int sock = -1;
    for (int attempt = 0; attempt < 3 && sock < 0; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }
    if (sock < 0) {
        logger_log(ESP_LOG_ERROR, TAG, "falha ao criar socket de dados apos 3 tentativas");
        goto cleanup_no_sock;
    }

    {
        struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* CRITICO (achado ao vivo, 2026-08-16, com backtrace real): connect()
     * bloqueante, sem timeout proprio -- SO_RCVTIMEO acima NAO limita
     * connect() no lwIP, so recv(). Sob disputa de radio/rede, connect()
     * podia ficar preso por varios segundos. Enquanto isso, s_data_sock so
     * e atualizado DEPOIS do connect() ter sucesso (linha abaixo) -- entao
     * um pulo de faixa rapido chegando nesse meio-tempo nao tinha como
     * fechar essa conexao pendente, e o timeout de 200ms do semaforo em
     * start_stream_session() (ver s_data_task_exited_sem) sempre expirava
     * achando que a task antiga tinha sumido, quando na verdade ela
     * continuava viva, presa aqui dentro -- reabrindo a MESMA corrida de
     * reuso de stack que o semaforo foi feito pra evitar, so que via
     * connect() lento em vez de falta de tempo pra IDLE. Quando o connect()
     * atrasado finalmente completava (ou uma resposta de rede tardia
     * chegava), o retorno mexia num stack ja reutilizado pela task nova --
     * causa real de pelo menos 2 crashes distintos decodificados essa noite
     * (ambos dentro de lwip_netconn_do_connected/xQueueGenericSend).
     * Corrigido com o padrao classico non-blocking connect + select() com
     * timeout real e curto (5s) -- garante que esta task NUNCA fica presa
     * aqui por mais que isso, então o semaforo sempre reflete a realidade. */
    int sock_flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, sock_flags | O_NONBLOCK);
    int connect_ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (connect_ret != 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval connect_tv = {.tv_sec = 5, .tv_usec = 0};
        int sel = select(sock + 1, NULL, &wfds, NULL, &connect_tv);
        if (sel > 0) {
            int so_error = 0;
            socklen_t so_error_len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
            connect_ret = (so_error == 0) ? 0 : -1;
        } else {
            connect_ret = -1; /* timeout ou erro no select() */
        }
    }
    fcntl(sock, F_SETFL, sock_flags); /* volta pro modo bloqueante -- resto do codigo espera isso */
    if (connect_ret != 0) {
        ESP_LOGW(TAG, "falha ao conectar no socket de dados (porta %d)", ntohs(addr.sin_port));
        goto cleanup;
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    s_data_sock = sock;
    xSemaphoreGive(s_data_mutex);

    if (!send_all(sock, (const uint8_t *)args->http_header, args->http_header_len)) {
        ESP_LOGW(TAG, "falha ao enviar cabecalho HTTP pro socket de dados");
        goto cleanup;
    }

    /* Consome a resposta HTTP (status + headers) até a linha em branco,
     * guardando o texto pra procurar "Transfer-Encoding: chunked" -- o
     * corpo pode vir em chunks (comum pra um stream gerado ao vivo, de
     * tamanho desconhecido, exatamente o caso aqui). */
    http_body_reader_t reader = {.chunked = false, .chunk_remaining = 0, .need_chunk_size = true};
    {
        char header_text[512];
        size_t header_len = 0;
        int crlf_run = 0;
        while (crlf_run < 4) {
            char c;
            int n = recv(sock, &c, 1, 0);
            if (n <= 0) {
                ESP_LOGW(TAG, "conexao de dados fechada durante o cabecalho HTTP");
                goto cleanup;
            }
            if (header_len < sizeof(header_text) - 1) {
                header_text[header_len++] = c;
            }
            crlf_run = (c == '\r' || c == '\n') ? crlf_run + 1 : 0;
        }
        header_text[header_len] = '\0';
        reader.chunked = contains_ci(header_text, "chunked");
        if (reader.chunked) {
            logger_log(ESP_LOG_INFO, TAG, "Slimproto: corpo HTTP chunked, desembrulhando");
        }
    }

    uint8_t stream_magic[4];
    if (!body_read_n(sock, &reader, stream_magic, sizeof(stream_magic))) {
        ESP_LOGW(TAG, "conexao de dados fechada logo no inicio do audio");
        goto cleanup;
    }
    logger_log(ESP_LOG_INFO, TAG, "Slimproto: primeiros 4 bytes do corpo: %02x %02x %02x %02x ('%c%c%c%c')",
               stream_magic[0], stream_magic[1], stream_magic[2], stream_magic[3],
               isprint(stream_magic[0]) ? stream_magic[0] : '.', isprint(stream_magic[1]) ? stream_magic[1] : '.',
               isprint(stream_magic[2]) ? stream_magic[2] : '.', isprint(stream_magic[3]) ? stream_magic[3] : '.');

    /* O campo "format" que o proprio STRM declara NAO e confiavel: bug
     * conhecido do Music Assistant (extracao de mime_type quebrada pra URLs
     * de streaming sem extensao de arquivo -- ver docs/music_assistant_
     * integration.md) faz ele por vezes declarar 'p' (PCM) no STRM mas
     * mandar FLAC de verdade no corpo (confirmado ao vivo: STRM dizia
     * format='p', corpo comecava com o magic "fLaC"). Os 4 primeiros bytes
     * reais do corpo sao a unica fonte confiavel -- "fLaC" e o magic number
     * oficial do formato (spec do Xiph, sempre os 4 primeiros bytes de um
     * stream FLAC nativo) -- entao decidimos o caminho de decodificacao por
     * eles, nao pelo que o servidor alegou no cabecalho Slimproto. */
    if (memcmp(stream_magic, "fLaC", 4) != 0) {
        int sample_rate = pcm_rate_to_hz(args->pcm_sample_rate);
        bool mono = (args->pcm_channels == '1');
        bool big_endian = (args->pcm_endian == '0');
        size_t bytes_per_sample_in = pcm_bytes_per_sample(args->pcm_sample_size);

        /* frame_buf guarda ate 256 bytes novos + a sobra (amostra
         * incompleta) do recv() anterior, quando frame_size_in nao divide
         * exatamente o tamanho lido (ex.: amostras de 3 bytes). out e
         * dimensionado pro pior caso (256 quadros de 1 byte, mono 8-bit). */
        uint8_t frame_buf[256 + 8];
        uint8_t out[256 * 4];
        size_t carry_len = 0;

        /* O Music Assistant manda WAV de verdade agora (nao PCM cru), com
         * cabecalho RIFF/WAVE + chunks (fmt, as vezes LIST/outros) antes do
         * chunk "data" -- e so DEPOIS dele que comeca audio de verdade. Sem
         * pular isso certinho, os bytes do cabecalho entram como se fossem
         * amostras, desalinhando TUDO dali pra frente (foi a causa real de
         * um chiado constante, cobrindo a faixa inteira, nao so um click
         * no comeco -- o cabecalho real tem chunks extras que não são
         * multiplos do tamanho do quadro, entao o desalinhamento nunca se
         * corrige sozinho). Se nao vier "RIFF" (servidor mandando PCM cru
         * de verdade), os 4 bytes lidos aqui SAO audio -- entram como sobra
         * pro loop principal processar, sem descartar nada. */
        {
            if (memcmp(stream_magic, "RIFF", 4) == 0) {
                uint8_t skip8[8]; /* riff_size(4) + "WAVE"(4) */
                if (!body_read_n(sock, &reader, skip8, sizeof(skip8))) {
                    goto cleanup;
                }
                for (;;) {
                    uint8_t chunk_hdr[8]; /* chunk_id(4) + chunk_size(4, little-endian) */
                    if (!body_read_n(sock, &reader, chunk_hdr, sizeof(chunk_hdr))) {
                        goto cleanup;
                    }
                    uint32_t chunk_size = (uint32_t)chunk_hdr[4] | ((uint32_t)chunk_hdr[5] << 8) |
                                           ((uint32_t)chunk_hdr[6] << 16) | ((uint32_t)chunk_hdr[7] << 24);
                    logger_log(ESP_LOG_INFO, TAG, "Slimproto: chunk WAV '%c%c%c%c' tamanho=%u",
                               isprint(chunk_hdr[0]) ? chunk_hdr[0] : '.', isprint(chunk_hdr[1]) ? chunk_hdr[1] : '.',
                               isprint(chunk_hdr[2]) ? chunk_hdr[2] : '.', isprint(chunk_hdr[3]) ? chunk_hdr[3] : '.',
                               (unsigned)chunk_size);
                    if (memcmp(chunk_hdr, "data", 4) == 0) {
                        break; /* audio comeca logo em seguida */
                    }
                    if (memcmp(chunk_hdr, "fmt ", 4) == 0 && chunk_size >= 16) {
                        /* WAVEFORMATEX: wFormatTag(2) nChannels(2) nSamplesPerSec(4)
                         * nAvgBytesPerSec(4) nBlockAlign(2) wBitsPerSample(2) [...]
                         * -- o formato real do arquivo pode nao bater com o que o
                         * cabecalho Slimproto anunciou (confirmado na pratica: STRM
                         * dizia 16-bit, o fmt chunk de verdade dizia 24-bit -- causa
                         * real do chiado/lentidao, amostras de 3 bytes lidas como
                         * se fossem de 2). Preferimos sempre o que o proprio WAV diz. */
                        uint8_t fmt_data[16];
                        if (!body_read_n(sock, &reader, fmt_data, sizeof(fmt_data))) {
                            goto cleanup;
                        }
                        uint16_t real_channels = (uint16_t)(fmt_data[2] | (fmt_data[3] << 8));
                        uint32_t real_rate = (uint32_t)fmt_data[4] | ((uint32_t)fmt_data[5] << 8) |
                                              ((uint32_t)fmt_data[6] << 16) | ((uint32_t)fmt_data[7] << 24);
                        uint16_t real_bits = (uint16_t)(fmt_data[14] | (fmt_data[15] << 8));
                        logger_log(ESP_LOG_INFO, TAG, "Slimproto: fmt real do WAV: %u canal(is), %u Hz, %u bits",
                                   real_channels, (unsigned)real_rate, real_bits);
                        if (real_channels == 1 || real_channels == 2) {
                            mono = (real_channels == 1);
                        }
                        if (real_rate > 0) {
                            sample_rate = (int)real_rate;
                        }
                        if (real_bits == 8 || real_bits == 16 || real_bits == 24 || real_bits == 32) {
                            bytes_per_sample_in = real_bits / 8;
                        }
                        uint32_t remaining = (chunk_size - 16) + (chunk_size & 1);
                        uint8_t trash[64];
                        while (remaining > 0) {
                            size_t want = remaining < sizeof(trash) ? remaining : sizeof(trash);
                            if (!body_read_n(sock, &reader, trash, want)) {
                                goto cleanup;
                            }
                            remaining -= (uint32_t)want;
                        }
                        continue;
                    }
                    uint32_t to_skip = chunk_size + (chunk_size & 1); /* chunks tem padding pra tamanho par */
                    uint8_t trash[64];
                    while (to_skip > 0) {
                        size_t want = to_skip < sizeof(trash) ? to_skip : sizeof(trash);
                        if (!body_read_n(sock, &reader, trash, want)) {
                            goto cleanup;
                        }
                        to_skip -= (uint32_t)want;
                    }
                }
            } else {
                /* nao e WAV -- ja e PCM cru, os 4 bytes lidos sao audio de verdade */
                memcpy(frame_buf, stream_magic, sizeof(stream_magic));
                carry_len = sizeof(stream_magic);
            }
        }

        /* Calculado so agora, depois do fmt chunk do WAV (se veio) poder ter
         * corrigido mono/bytes_per_sample_in em relacao ao que o cabecalho
         * Slimproto anunciou. */
        size_t channels_in = mono ? 1 : 2;
        size_t frame_size_in = bytes_per_sample_in * channels_in;

        audio_codec_reconfigure_clock((uint32_t)sample_rate);
        s_slim_sample_rate = (uint32_t)sample_rate;
        logger_log(ESP_LOG_INFO, TAG, "Slimproto: stream iniciado (%d Hz, %u bits, %s, %s-endian)",
                   sample_rate, (unsigned)(bytes_per_sample_in * 8), mono ? "mono" : "estereo",
                   big_endian ? "big" : "little");

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.playing = true;
        xSemaphoreGive(s_status_mutex);
        relay_control_notify_playing(true);
        audio_codec_set_mute(false);
        /* Confirmado no source do aioslimproto (client.py, _process_STMs):
         * "Playback of new track has started" -- usado por eles pra saber
         * a hora certa de parar de confiar no tempo decorrido da faixa
         * ANTERIOR (que ainda pode mandar STMt "presos" chegando atrasados)
         * e passar a confiar na faixa atual. Sem mandar isso, o tempo
         * decorrido mostrado no MA pode ficar errado/atrasado na troca de
         * faixa mesmo com o audio certo tocando. */
        send_stat_safe("STMs", 0);

        for (;;) {
            if (s_data_paused) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            int n = http_body_read(sock, &reader, frame_buf + carry_len, 256);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                    /* SO_RCVTIMEO estourou so porque o servidor teve um
                     * intervalo natural sem mandar dado (buffer/rede) --
                     * NAO e fim de faixa. Tratar isso como erro derrubava a
                     * sessao (mutava, desligava o rele, zerava o status de
                     * "tocando") no meio de reproducoes que continuavam
                     * normais do ponto de vista do servidor. */
                    continue;
                }
                break; /* erro de verdade (ex.: stop_stream_session fechou o socket) */
            }
            if (n == 0) {
                /* EOF limpo (servidor fechou de proposito, fim da faixa) --
                 * CORRECAO (visto o corpo real de _process_stat_stmd/stmu no
                 * client.py do aioslimproto, nao so o docstring): quem
                 * dispara a proxima faixa pre-carregada (enqueue_next_media)
                 * e o handler do STMd -- "if self._next_media: play_url(...)".
                 * STMu faz o OPOSTO do que precisamos aqui: joga
                 * self._next_media pra None (junto com state=STOPPED),
                 * cancelando exatamente a faixa que MA ja tinha preparado.
                 * Mandar STMu no EOF (como o commit anterior fazia) travava
                 * o avanco automatico em vez de destravar. */
                bool sent_d = send_stat_safe("STMd", 0);
                logger_log(ESP_LOG_INFO, TAG, "Slimproto: fim natural da faixa, avisando servidor (STMd=%d)",
                           (int)sent_d);
                break;
            }

            /* BT sempre tem prioridade sobre o Slimproto: se estiver
             * conectado, descartamos o audio em vez de brigar pelo mesmo
             * I2S. Ver slimproto.h. */
            bt_audio_status_t bt;
            bt_audio_get_status(&bt);
            if (bt.connected) {
                carry_len = 0; /* descarta tambem a sobra -- nao faz sentido remontar depois de um gap */
                continue;
            }

            size_t total = carry_len + (size_t)n;
            size_t frames = total / frame_size_in;
            if (frames > sizeof(out) / 4) {
                frames = sizeof(out) / 4; /* nao deveria acontecer com os tamanhos atuais, so protege o buffer */
            }
            size_t usable_bytes = frames * frame_size_in;

            for (size_t f = 0; f < frames; f++) {
                const uint8_t *left = frame_buf + f * frame_size_in;
                const uint8_t *right = mono ? left : left + bytes_per_sample_in;
                int16_t l = pcm_extract_16bit(left, bytes_per_sample_in, big_endian);
                int16_t r = pcm_extract_16bit(right, bytes_per_sample_in, big_endian);
                out[f * 4 + 0] = (uint8_t)(l & 0xff);
                out[f * 4 + 1] = (uint8_t)((l >> 8) & 0xff);
                out[f * 4 + 2] = (uint8_t)(r & 0xff);
                out[f * 4 + 3] = (uint8_t)((r >> 8) & 0xff);
            }

            slim_write_ringbuf(my_session, out, frames * 4);

            /* guarda o resto (quadro incompleto) pro proximo recv() completar */
            size_t new_carry_len = total - usable_bytes;
            if (new_carry_len > 0) {
                memmove(frame_buf, frame_buf + usable_bytes, new_carry_len);
            }
            carry_len = new_carry_len;
        }
    } else {
        /* FLAC nativo (detectado pelo magic "fLaC" real do corpo, nao pelo
         * campo "format" do STRM -- ver comentario acima) -- decodificado
         * localmente via esphome/micro-flac em vez de exigir PCM/WAV do
         * servidor. Elimina de vez a dependencia de qualquer patch no lado
         * do Music Assistant (a extracao de mime_type dele e quebrada pra
         * URLs de streaming sem extensao de arquivo, entao ele volta a
         * mandar FLAC -- o formato padrao -- com muita frequencia mesmo com
         * CONF_OUTPUT_CODEC setado; ver docs/music_assistant_integration.md).
         *
         * Buffer de ENTRADA (bytes comprimidos, crus do socket): mantido o
         * mais cheio possivel -- le mais dados so quando ha espaco livre,
         * decode() consome do inicio e a sobra e compactada pra frente via
         * memmove(). 32KB cobre com folga um frame FLAC tipico (bloco de
         * 4096 amostras/canal, estereo, ate 24-bit VERBATIM no pior caso:
         * 4096*2*3 = 24576 bytes).
         *
         * Buffer de SAIDA (amostras int32_t "left-justified", formato
         * uniforme da lib independente da profundidade real do FLAC):
         * alocado só depois do FLAC_STREAM_HEADER_READY, do tamanho exato
         * que o proprio decoder informa (get_output_buffer_size_samples()).
         *
         * Os dois ficam em PSRAM (MALLOC_CAP_SPIRAM) -- sao grandes demais
         * pra RAM interna, que ja e disputada por WiFi/BT (ver o comentario
         * sobre isso em main.c). */
        flac_stream_decoder_t *flac = flac_stream_decoder_create();
        if (flac == NULL) {
            ESP_LOGE(TAG, "falha ao criar decoder FLAC (sem heap)");
            goto cleanup;
        }

        const size_t FLAC_IN_CAP = 32768;
        uint8_t *in_buf = heap_caps_malloc(FLAC_IN_CAP, MALLOC_CAP_SPIRAM);
        if (in_buf == NULL) {
            ESP_LOGE(TAG, "falha ao alocar buffer de entrada FLAC (sem PSRAM)");
            flac_stream_decoder_destroy(flac);
            goto cleanup;
        }
        /* os 4 bytes de magic ja foram consumidos do socket acima (pra
         * decidir este ramo) -- entram como os primeiros bytes de entrada
         * do decoder, senao ele nunca veria o "fLaC" inicial. */
        memcpy(in_buf, stream_magic, sizeof(stream_magic));
        size_t in_len = sizeof(stream_magic);

        int32_t *out_buf = NULL;
        size_t out_cap_samples = 0;
        uint32_t channels = 2;
        bool clean_end = false;
        bool socket_eof = false;
        unsigned yield_counter = 0;

        for (;;) {
            if (s_data_paused) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            /* Cede a vez de verdade (vTaskDelay, nao taskYIELD -- so um
             * delay real tira a task da fila de prontas e deixa a IDLE
             * rodar) a cada 32 voltas. Necessario pra fixar esta task num
             * nucleo so (ver xTaskCreateStaticPinnedToCore mais abaixo):
             * quando in_len==FLAC_IN_CAP, o laco roda decode+escrita no
             * ring buffer em sequencia sem nenhuma chamada bloqueante --
             * sem isso, a IDLE do nucleo nunca escalona e o watchdog de
             * interrupcao mata o sistema (ja aconteceu, ver comentario
             * junto do xTaskCreateStaticPinnedToCore). 32 voltas a ~poucas
             * dezenas de us cada e um orcamento de CPU desprezivel perto
             * do buffer de ~1s que o ring buffer da de folga.*/
            if (++yield_counter >= 32) {
                yield_counter = 0;
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            if (!socket_eof && in_len < FLAC_IN_CAP) {
                int n = http_body_read(sock, &reader, in_buf + in_len, FLAC_IN_CAP - in_len);
                if (n < 0) {
                    if (!(errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)) {
                        break; /* erro real de socket */
                    }
                    /* timeout sem dado novo -- ainda tenta decodificar o que ja tem */
                } else if (n == 0) {
                    socket_eof = true; /* EOF limpo -- drena o que sobrou no buffer/decoder abaixo */
                } else {
                    in_len += (size_t)n;
                }
            }

            size_t consumed = 0, decoded = 0;
            flac_stream_result_t r = flac_stream_decoder_feed(flac, in_buf, in_len, out_buf,
                                                                out_cap_samples, &consumed, &decoded);
            if (consumed > 0) {
                memmove(in_buf, in_buf + consumed, in_len - consumed);
                in_len -= consumed;
            }

            if (r == FLAC_STREAM_HEADER_READY) {
                channels = flac_stream_decoder_channels(flac);
                uint32_t rate = flac_stream_decoder_sample_rate(flac);
                out_cap_samples = flac_stream_decoder_output_buffer_samples(flac);
                out_buf = heap_caps_malloc(out_cap_samples * sizeof(int32_t), MALLOC_CAP_SPIRAM);
                if (out_buf == NULL) {
                    ESP_LOGE(TAG, "falha ao alocar buffer de saida FLAC (sem PSRAM)");
                    break;
                }
                audio_codec_reconfigure_clock(rate);
                s_slim_sample_rate = rate;
                logger_log(ESP_LOG_INFO, TAG, "Slimproto: FLAC iniciado (%u Hz, %u bits, %u canal(is))",
                           (unsigned)rate, (unsigned)flac_stream_decoder_bits_per_sample(flac),
                           (unsigned)channels);

                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                s_status.playing = true;
                xSemaphoreGive(s_status_mutex);
                relay_control_notify_playing(true);
                audio_codec_set_mute(false);
                send_stat_safe("STMs", 0); /* ver explicacao no ramo PCM acima */
                continue;
            }

            if (r == FLAC_STREAM_OK) {
                if (decoded > 0 && channels > 0) {
                    bt_audio_status_t bt;
                    bt_audio_get_status(&bt);
                    if (!bt.connected) {
                        size_t total_frames = decoded / channels;
                        size_t frame_idx = 0;
                        while (frame_idx < total_frames) {
                            size_t chunk_frames = total_frames - frame_idx;
                            if (chunk_frames > 256) {
                                chunk_frames = 256;
                            }
                            uint8_t chunk16[256 * 4];
                            for (size_t f = 0; f < chunk_frames; f++) {
                                const int32_t *frame = out_buf + (frame_idx + f) * channels;
                                /* amostras vem alinhadas a esquerda em 32 bits -- os 16
                                 * bits mais significativos ja sao o truncamento pra
                                 * 16-bit, igual ao pcm_extract_16bit() do ramo PCM. */
                                int16_t l = (int16_t)(frame[0] >> 16);
                                int16_t rr = (channels >= 2) ? (int16_t)(frame[1] >> 16) : l;
                                chunk16[f * 4 + 0] = (uint8_t)(l & 0xff);
                                chunk16[f * 4 + 1] = (uint8_t)((l >> 8) & 0xff);
                                chunk16[f * 4 + 2] = (uint8_t)(rr & 0xff);
                                chunk16[f * 4 + 3] = (uint8_t)((rr >> 8) & 0xff);
                            }
                            slim_write_ringbuf(my_session, chunk16, chunk_frames * 4);
                            frame_idx += chunk_frames;
                        }
                    }
                }
                continue;
            }

            if (r == FLAC_STREAM_END_OF_STREAM) {
                clean_end = true;
                break;
            }

            if (r == FLAC_STREAM_ERROR) {
                logger_log(ESP_LOG_WARN, TAG, "Slimproto: erro no decoder FLAC, encerrando faixa");
                break;
            }

            /* FLAC_STREAM_NEED_MORE_DATA */
            if (socket_eof) {
                /* servidor ja fechou a conexao e o decoder ainda pede mais --
                 * stream truncado ou o ultimo frame ja foi totalmente
                 * consumido; de qualquer forma, nao ha mais nada a fazer. */
                clean_end = true;
                break;
            }
            if (in_len == FLAC_IN_CAP && consumed == 0) {
                logger_log(ESP_LOG_ERROR, TAG, "Slimproto: buffer de entrada FLAC saturado, abortando faixa");
                break;
            }
            /* buffer de entrada ainda tem espaco (ou o decoder consumiu algo) -- volta pro topo do loop */
        }

        if (clean_end) {
            bool sent_d = send_stat_safe("STMd", 0);
            logger_log(ESP_LOG_INFO, TAG, "Slimproto: fim natural da faixa (FLAC), avisando servidor (STMd=%d)",
                       (int)sent_d);
        }

        if (out_buf != NULL) {
            heap_caps_free(out_buf);
        }
        heap_caps_free(in_buf);
        flac_stream_decoder_destroy(flac);
    }

cleanup:
    /* So remuda se esta sessao ainda for a atual -- se uma faixa nova ja
     * comecou (skip rapido), a task dela ja desmutou o codec, e mutar aqui
     * incondicionalmente corta o comeco da faixa nova numa corrida sem
     * ordem garantida entre "task antiga terminando" e "task nova
     * comecando" (era exatamente esse race o "engasgo" residual ao pular
     * de faixa, mesmo depois do buffer e do vazamento de audio corrigidos). */
    if (my_session == s_slim_session_id) {
        audio_codec_set_mute(true);
    }
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (s_data_sock == sock) {
        s_data_sock = -1;
    }
    xSemaphoreGive(s_data_mutex);
    close(sock);
cleanup_no_sock:
    /* NAO mexemos em s_status.playing/rele aqui de proposito: o fim desta
     * conexao de dados especifica nao significa "parou de tocar" -- o MA
     * abre uma conexao de dados nova a cada trecho/faixa (STRM 's' de novo),
     * entao isso disparava a cada poucos segundos mesmo com audio continuo,
     * fazendo o status "tocando" piscar false toda hora (visto direto pelo
     * usuario: audio real continuava, /api/status quase nunca pegava true).
     * Quem decide "playing=false" de verdade agora e so um STOP/FLUSH
     * explicito (handle_strm) ou a conexao de CONTROLE cair de vez
     * (slimproto_control_task). */
    /* Telemetria real (nao mais chute) de quanto stack esta task realmente
     * precisou -- uxTaskGetStackHighWaterMark() devolve o MINIMO de bytes
     * livres que a task teve em toda sua vida (quanto menor, mais perto
     * chegou de estourar). Visivel em /api/logs -- decide se os 10KB atuais
     * tem margem de sobra ou se e melhor subir um pouco. */
    logger_log(ESP_LOG_INFO, TAG, "slim_data: margem de stack no pior momento desta sessao: %u bytes",
               (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(args);
    /* Ultima coisa antes de sair de vez -- sinaliza pra um eventual
     * start_stream_session() esperando (ver s_data_task_exited_sem) que ja
     * e seguro reusar o static stack/TCB agora. */
    xSemaphoreGive(s_data_task_exited_sem);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Comandos recebidos na conexão de controle
 * ------------------------------------------------------------------------- */

static void handle_strm(int ctrl_sock, const uint8_t *payload, size_t len)
{
    if (len < 24) {
        ESP_LOGW(TAG, "strm curto demais (%u bytes)", (unsigned)len);
        return;
    }

    char command = (char)payload[0];
    char format = (char)payload[2];
    char pcm_size = (char)payload[3];
    char pcm_rate = (char)payload[4];
    char pcm_channels = (char)payload[5];
    char pcm_endian = (char)payload[6];
    uint16_t server_port = get_u16(payload + 18);
    uint32_t server_ip = get_u32(payload + 20);

    /* Diagnostico: registra strm recebido no logger visivel (/api/logs) --
     * antes so ia pro ESP_LOGW cru (so serial), entao a rejeicao de formato
     * ficava invisivel remotamente. Exclui 'cmd==t' (heartbeat, repete a
     * cada poucos segundos pra sempre) de proposito -- sem isso, o buffer
     * circular de 100 entradas ficava tomado so por heartbeats em minutos,
     * apagando informacao bem mais relevante (troca de sessao, motivo do
     * reset no boot, etc.) rapido demais pra dar tempo de puxar via API. */
    if (command != 't') {
        logger_log(ESP_LOG_INFO, TAG, "strm recebido: cmd='%c' format='%c' pcm_size='%c' pcm_rate='%c' pcm_channels='%c'",
                   command, format, pcm_size, pcm_rate, pcm_channels);
    }

    switch (command) {
        case 's': /* start */
            if (format != 'p' && format != 'f') {
                logger_log(ESP_LOG_WARN, TAG, "formato '%c' nao suportado (so PCM/FLAC), ignorando faixa", format);
                send_stat(ctrl_sock, "STMn", 0);
                break;
            }
            start_stream_session(htonl(server_ip), server_port, payload + 24, len - 24, format,
                                  pcm_size, pcm_rate, pcm_channels, pcm_endian);
            break;
        case 'p': /* pause */
            s_data_paused = true;
            audio_codec_set_mute(true);
            /* s_status.playing nunca era atualizado aqui -- ficava travado
             * no valor de quando a faixa comecou, entao lms_cli_send_
             * transport() (que decide "pause 1" vs "pause 0" pro botao de
             * alternar) sempre via o mesmo estado antigo e mandava sempre
             * o mesmo comando, travando o toggle. Confirmado ao vivo em
             * 2026-08-14. */
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.playing = false;
            xSemaphoreGive(s_status_mutex);
            break;
        case 'u': /* unpause */
            s_data_paused = false;
            audio_codec_set_mute(false);
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.playing = true;
            xSemaphoreGive(s_status_mutex);
            break;
        case 'q': /* stop */
        case 'f': /* flush */
            stop_stream_session();
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_status.playing = false;
            xSemaphoreGive(s_status_mutex);
            relay_control_notify_playing(false);
            send_stat(ctrl_sock, "STMf", 0);
            break;
        case 't': { /* status/time request -- ecoa o timestamp do servidor (campo replay_gain) */
            uint32_t ts = get_u32(payload + 14);
            send_stat(ctrl_sock, "STMt", ts);
            break;
        }
        default:
            break;
    }
}

/* O servidor pergunta o nome do player (id 0x00) logo depois do HELO --
 * sem responder, ele usa um nome generico ("squeezeplay: <mac>"). Outros
 * ids (ex.: 0xfe, nao documentado) sao so ignorados, como o squeezelite
 * tambem faz. */
static void handle_setd(int sock, const uint8_t *payload, size_t len)
{
    if (len < 1 || payload[0] != 0x00) {
        return;
    }

    char device_name[32];
    if (storage_get_str(NVS_KEY_DEVICE_NAME, device_name, sizeof(device_name)) != ESP_OK || device_name[0] == '\0') {
        strlcpy(device_name, FW_DEVICE_NAME_DEFAULT, sizeof(device_name));
    }

    uint8_t resp[1 + sizeof(device_name)];
    resp[0] = 0x00;
    size_t name_len = strlen(device_name);
    memcpy(resp + 1, device_name, name_len);
    resp[1 + name_len] = '\0';
    send_message(sock, "setd", resp, 1 + name_len + 1);
}

/* Ajuste de volume vindo do servidor (ex.: controle deslizante do Music
 * Assistant, ou o botao de mudo dele) -- confirmado: sem tratar esse
 * opcode, o volume/mudo do MA nao tinha NENHUM efeito no dispositivo,
 * porque handle_ctrl_message() descartava "audg" junto com todo opcode que
 * nao fosse strm/setd. Payload (18 bytes, ver squeezelite/slimproto real):
 * old_gainL(4) old_gainR(4) adjust(1) preamp(1) gainL(4) gainR(4), gains em
 * ponto fixo 16.16 (65536 = 0dB/ganho unitario). Usamos so o canal
 * esquerdo -- nosso volume e um unico valor pros dois canais. */
static void handle_audg(const uint8_t *payload, size_t len)
{
    if (len < 18) {
        return;
    }
    uint32_t gain_l = get_u32(payload + 10);
    float linear = (float)gain_l / 65536.0f;
    if (linear > 1.0f) {
        linear = 1.0f;
    } else if (linear < 0.0f) {
        linear = 0.0f;
    }
    int volume = (int)(linear * VOLUME_STEPS + 0.5f);
    audio_codec_set_volume(volume);
    logger_log(ESP_LOG_INFO, TAG, "Slimproto: audg recebido (ganho=%.3f) -> volume %d/%d",
               linear, volume, VOLUME_STEPS);
}

/* -------------------------------------------------------------------------
 * Task de controle: conecta, manda HELO, processa comandos e manda
 * heartbeat (STAT/STMt) periodico. Reconecta sozinha se cair.
 * ------------------------------------------------------------------------- */

static bool recv_exact(int sock, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = recv(sock, buf + got, len - got, 0);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static void slimproto_control_task(void *arg)
{
    for (;;) {
        char host[64];
        bool has_host = (storage_get_str(NVS_KEY_SLIM_HOST, host, sizeof(host)) == ESP_OK) && host[0] != '\0';

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.enabled = has_host;
        strlcpy(s_status.server_host, has_host ? host : "", sizeof(s_status.server_host));
        xSemaphoreGive(s_status_mutex);

        if (!has_host) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        char ip_str[16];
        wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
        if (ip_str[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
        struct addrinfo *res = NULL;
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%d", SLIMPROTO_SERVER_PORT);
        if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
            ESP_LOGW(TAG, "nao foi possivel resolver '%s'", host);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) {
            freeaddrinfo(res);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        s_control_server_ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;

        bool ok = (connect(sock, res->ai_addr, res->ai_addrlen) == 0);
        freeaddrinfo(res);

        if (!ok) {
            ESP_LOGW(TAG, "falha ao conectar em %s:%d", host, SLIMPROTO_SERVER_PORT);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        /* SO_KEEPALIVE ausente ate agora -- a conexao de CONTROLE (esta
         * aqui) e a que fica aberta a vida toda, mas nunca tinha nenhuma
         * deteccao de peer morto alem do proprio STMt de 5 em 5s falhar no
         * envio, o que pode demorar muito (retransmissao TCP padrao) ou
         * nunca acontecer de verdade se a rede so parou de entregar pacotes
         * silenciosamente (NAT/roteador reiniciando, WiFi soluçando) sem
         * nunca mandar um RST -- o socket fica "conectado" pro nosso lado
         * pra sempre, mas o MA ja desistiu do outro lado (confirmado ao vivo
         * em 2026-08-14: dispositivo mostrava slim_connected=true no nosso
         * /api/status enquanto o MA ja via o player como indisponivel).
         * Com keepalive ativo, idle=10s + intervalo=5s + 3 tentativas =
         * detectamos um peer morto em ate ~25s e a logica de reconexao ja
         * existente (ver "conexao perdida, tentando de novo" mais abaixo)
         * cuida do resto. */
        {
            int keepalive = 1;
            int keepidle = 10;
            int keepintvl = 5;
            int keepcnt = 3;
            setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
        }

        if (!send_helo(sock)) {
            ESP_LOGW(TAG, "falha ao enviar HELO");
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        logger_log(ESP_LOG_INFO, TAG, "Slimproto conectado em %s:%d", host, SLIMPROTO_SERVER_PORT);
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.connected = true;
        xSemaphoreGive(s_status_mutex);

        xSemaphoreTake(s_ctrl_mutex, portMAX_DELAY);
        s_ctrl_sock = sock;
        xSemaphoreGive(s_ctrl_mutex);

        send_stat(sock, "STMt", 0);
        int64_t last_heartbeat = esp_timer_get_time();

        bool alive = true;
        while (alive) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            struct timeval sel_tv = {.tv_sec = 1, .tv_usec = 0};
            int r = select(sock + 1, &rfds, NULL, NULL, &sel_tv);

            if (r < 0) {
                break;
            }

            if (r > 0 && FD_ISSET(sock, &rfds)) {
                /* Assimetria real do protocolo (confirmada capturando bytes
                 * crus de um servidor de verdade, nao só pela doc): mensagens
                 * CLIENTE->SERVOR (HELO/STAT, ver send_message) usam opcode de
                 * 4 bytes + tamanho de 4 bytes: mensagens SERVIDOR->CLIENTE
                 * (strm/vers/setd/aude/audg/...) usam so 2 bytes de tamanho
                 * (cobrindo opcode+payload juntos) NA FRENTE do opcode. Usar o
                 * framing de 8 bytes pros dois lados (erro original) lia 2
                 * bytes a mais do que devia logo na primeira mensagem, e
                 * desincronizava tudo dali pra frente. */
                uint8_t len_hdr[2];
                if (!recv_exact(sock, len_hdr, sizeof(len_hdr))) {
                    break;
                }
                uint16_t total_len = get_u16(len_hdr); /* = 4 (opcode) + tamanho do payload */
                if (total_len < 4 || total_len > 4 + MAX_CTRL_PAYLOAD) {
                    ESP_LOGW(TAG, "mensagem com tamanho invalido (%u), desconectando", (unsigned)total_len);
                    break;
                }
                uint8_t msg[4 + MAX_CTRL_PAYLOAD];
                if (!recv_exact(sock, msg, total_len)) {
                    break;
                }
                size_t plen = total_len - 4;
                if (memcmp(msg, "strm", 4) == 0) {
                    handle_strm(sock, msg + 4, plen);
                } else if (memcmp(msg, "setd", 4) == 0) {
                    handle_setd(sock, msg + 4, plen);
                } else if (memcmp(msg, "audg", 4) == 0) {
                    handle_audg(msg + 4, plen);
                }
                /* qualquer outro opcode (aude/vers/... -- aude controla
                 * canais de saida individuais de hardware multi-saida, nao
                 * se aplica aqui) so e descartado -- ja consumimos
                 * exatamente `total_len` bytes acima, entao o framing da
                 * conexao continua sincronizado. */
            }

            int64_t now = esp_timer_get_time();
            if (now - last_heartbeat > HEARTBEAT_INTERVAL_US) {
                if (!send_stat(sock, "STMt", 0)) {
                    break;
                }
                last_heartbeat = now;
            }
        }

        logger_log(ESP_LOG_WARN, TAG, "Slimproto: conexao perdida, tentando de novo");
        stop_stream_session();
        xSemaphoreTake(s_ctrl_mutex, portMAX_DELAY);
        s_ctrl_sock = -1;
        xSemaphoreGive(s_ctrl_mutex);
        close(sock);
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.connected = false;
        bool was_playing = s_status.playing;
        s_status.playing = false;
        xSemaphoreGive(s_status_mutex);
        if (was_playing) {
            /* So mexe no rele se a gente REALMENTE estava tocando algo --
             * senao, cada reconexao (frequente com a rede instavel)
             * reiniciava o timer de desligamento do zero pra sempre,
             * mesmo sem nunca ter tocado nada (ex.: colidindo com o bipe
             * de teste do boot, que liga o rele por conta propria). */
            relay_control_notify_playing(false);
        }
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

/* -------------------------------------------------------------------------
 * API pública
 * ------------------------------------------------------------------------- */

void slimproto_get_status(slimproto_status_t *out)
{
    if (out == NULL || s_status_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mutex);
}

int64_t slimproto_get_last_transition_time_us(void)
{
    return s_last_session_transition_us;
}

void slimproto_init(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    s_data_mutex = xSemaphoreCreateMutex();
    s_data_sock = -1;
    /* Binario, criado "vazio" (nao dado) -- correto: na primeiríssima
     * chamada de start_stream_session() apos o boot nao ha task anterior
     * nenhuma pra sinalizar, entao o take() la deve mesmo esperar o timeout
     * inteiro e seguir em frente, igual o vTaskDelay(50) fixo que existia
     * antes fazia sempre. */
    s_data_task_exited_sem = xSemaphoreCreateBinary();
    s_ctrl_mutex = xSemaphoreCreateMutex();
    s_ctrl_sock = -1;

    /* Pre-alocado uma unica vez, aqui no boot, junto com a task dedicada de
     * I2S -- mesmo motivo do bt_audio.c: evita competir por heap contra
     * WiFi/BT durante o meio de uma sessao de streaming. A task fica viva
     * a vida toda do dispositivo, bloqueada no semaforo quando ociosa. */
    s_slim_ringbuf_storage = heap_caps_malloc(SLIM_RINGBUF_HIGHEST_WATER_LEVEL, MALLOC_CAP_SPIRAM);
    if (s_slim_ringbuf_storage != NULL) {
        s_slim_ringbuf = xRingbufferCreateStatic(SLIM_RINGBUF_HIGHEST_WATER_LEVEL, RINGBUF_TYPE_BYTEBUF,
                                                  s_slim_ringbuf_storage, &s_slim_ringbuf_struct);
    }
    s_slim_i2s_sem = xSemaphoreCreateBinary();
    /* Prioridade quase maxima (igual ao bt_i2s_task do bt_audio.c, mesmo
     * raciocinio) -- confirmado ao vivo em 2026-08-14 que engasgos
     * ("picotes") continuavam mesmo com RAM/buffers saudaveis; comparar as
     * duas tasks revelou que o audio do Bluetooth ja usa prioridade quase
     * maxima (configMAX_PRIORITIES - 3) especificamente pra nao ser
     * atropelado pela task de WiFi (prioridade 23, fixa do driver), mas
     * essa aqui (que faz o mesmo trabalho -- drenar o ring buffer pro I2S
     * em tempo real) ficou esquecida em prioridade 6. Qualquer rajada de
     * WiFi/BT (coexistencia de radio, visivel nos avisos "wifi:m f null")
     * podia atrasar essa task tempo suficiente pra estourar a folga do
     * buffer DMA (~65ms, 12 descritores). */
    /* Prioridade moderada (7, so 2 acima do decode) -- configMAX_PRIORITIES-3
     * tentado antes (2026-08-14) causou um sintoma NOVO ("acelera/desacelera")
     * provavelmente porque, no MESMO nucleo do decode (slim_data, prioridade
     * 5), uma prioridade quase maxima podia sufocar o proprio decode que
     * alimenta o ring buffer -- preempcao excessiva no lugar errado. Ainda
     * mais alta que o decode (garante que drenar o buffer sempre vence
     * empate), mas sem sufocar quem o enche. */
    /* REVERTIDO o pin de nucleo junto com slim_data (ver comentario la --
     * travamento completo do dispositivo em 2026-08-14) -- mantido so o
     * ganho de prioridade (6->7), independente do pin. */
    xTaskCreate(slim_i2s_task_handler, "slim_i2s", 3072, NULL, 7, NULL);

    /* A task de dados (slim_data) e criada POR FAIXA em start_stream_session(),
     * nao aqui -- ver o comentario grande la sobre por que uma task
     * persistente unica (reservando 16KB de RAM interna desde o boot pra
     * sempre) chegou a afogar o MQTT e o proprio driver de WiFi por falta
     * de memoria, mesmo antes de qualquer faixa tocar. */

    /* Prioridade 6 (nao 4): igual a task de I2S, e ACIMA da task de dados
     * (slim_data, prioridade 5). Essa task responde ao heartbeat "strm t"
     * periodico do servidor (STMt) -- decodificacao FLAC (slim_data,
     * trabalho pesado de CPU em C++, sem ceder o processador com a mesma
     * frequencia que o antigo caminho PCM cru cedia) rodando numa
     * prioridade MAIOR que essa podia atrasar a resposta ao heartbeat o
     * suficiente pro Music Assistant marcar o player como indisponivel
     * temporariamente (observado ao vivo: dispositivo "desativado" no MA
     * mesmo respondendo normal via /api/status, e um reinicio de stream
     * logo no comeco de uma faixa). O trabalho desta task e sempre curto
     * (parsear poucos bytes, mandar uma resposta pequena, voltar a
     * bloquear em recv()), entao rodar numa prioridade alta nao rouba CPU
     * de ninguem por muito tempo. */
    xTaskCreate(slimproto_control_task, "slim_ctrl", 4096, NULL, 6, NULL);
}
