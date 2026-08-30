#include "es8388.h"

#include "config.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8388";

/* 0x10 (7 bits) — confirmado no bring-up real: 0x20 (a forma de 8 bits,
 * já deslocada, usada em exemplos com o driver I2C legado) falhava toda
 * escrita de registrador com o driver i2c_master novo do IDF, que espera
 * o endereço de 7 bits e faz o shift internamente. */
#define ES8388_I2C_ADDR 0x10

/* Registradores usados (subconjunto — só o necessário para DAC/line-out). */
#define ES8388_CONTROL1   0x00
#define ES8388_CONTROL2   0x01
#define ES8388_CHIPPOWER  0x02
#define ES8388_ADCPOWER   0x03
#define ES8388_DACPOWER   0x04
#define ES8388_MASTERMODE 0x08
#define ES8388_DACCONTROL1  0x17
#define ES8388_DACCONTROL2  0x18
#define ES8388_DACCONTROL3  0x19
#define ES8388_DACCONTROL4  0x1a /* volume DAC direito */
#define ES8388_DACCONTROL5  0x1b /* volume DAC esquerdo */
#define ES8388_DACCONTROL16 0x26
#define ES8388_DACCONTROL17 0x27
#define ES8388_DACCONTROL20 0x2a
#define ES8388_DACCONTROL21 0x2b
#define ES8388_DACCONTROL23 0x2d
#define ES8388_DACCONTROL24 0x2e
#define ES8388_DACCONTROL25 0x2f
#define ES8388_DACCONTROL26 0x30
#define ES8388_DACCONTROL27 0x31

#define DAC_OUTPUT_ALL 0x3C /* LOUT1 | LOUT2 | ROUT1 | ROUT2 */

/* Registradores do ADC/microfone -- não usados no caminho de DAC acima. */
#define ES8388_ADCCONTROL1  0x09 /* ganho do PGA do mic (nibble alto = LIN, baixo = RIN) */
#define ES8388_ADCCONTROL2  0x0A /* seleção de entrada analógica */
#define ES8388_ADCCONTROL3  0x0B
#define ES8388_ADCCONTROL4  0x0C /* formato I2S do ADC */
#define ES8388_ADCCONTROL5  0x0D /* clock/oversampling do ADC */
#define ES8388_ADCCONTROL8  0x10 /* volume digital ADC esquerdo */
#define ES8388_ADCCONTROL9  0x11 /* volume digital ADC direito */
#define ES8388_ADCCONTROL10 0x12 /* ALC: selecao, ganho maximo e minimo */
#define ES8388_ADCCONTROL11 0x13 /* ALC: nivel alvo e tempo de espera */
#define ES8388_ADCCONTROL12 0x14 /* ALC: tempos de ataque e decaimento */
#define ES8388_ADCCONTROL13 0x15 /* ALC: modo e tamanho da janela */
#define ES8388_ADCCONTROL14 0x16 /* noise gate do proprio codec */

/* Entrada do ADC: 0xF0 (DIFERENCIAL) -- validado ao vivo, com audio limpo
 * confirmado pelo usuario (2026-08-29).
 *
 * QUEM CAPTA E O RECEPTOR SEM FIO DO USUARIO, nao o microfone embutido da
 * placa. Provado pelo unico teste com a fonte isolada de verdade -- ele
 * falando a distancia, primeiro com o receptor ligado, depois desligado:
 *
 *   receptor LIGADO,    ele falando -> media 1462, pico 7591
 *   receptor DESLIGADO, ele falando -> media   81
 *   piso em silencio ..................... media   77
 *
 * Com o receptor desligado, falar nao muda nada (81 contra piso de 77): o
 * microfone embutido praticamente nao o alcanca a essa distancia. So o
 * diferencial enxerga o receptor -- 0x50 e 0x00 nao saem do piso.
 *
 * O MICROFONE EMBUTIDO NAO ATRAPALHA A CAPTACAO, MAS LIMITA O GANHO. Ele so
 * domina quando alguem fala colado na placa -- foi exatamente essa a condicao
 * das medicoes erradas de 2026-08-28, que me fizeram trocar a entrada tres
 * vezes atras do numero maior em vez do numero que REAGE a fonte certa. O
 * problema real que ele causa e realimentacao: a partir de +17.5dB de ALC o
 * usuario ouve microfonia, com o microfone dele DESLIGADO. Por isso o ALC
 * esta preso em +11.5dB (ver es8388_mic_init) e o usuario ainda precisa
 * cantar perto do proprio microfone.
 *
 * SE O MICROFONE EMBUTIDO FOR DESSOLDADO: a captacao continua funcionando
 * (ela ja vem do receptor), e o teto de ganho sobe -- e ganho e justamente o
 * que falta. Vale recalibrar ALC, PGA e o limiar do portao juntos, e
 * revalidar as tres entradas com o protocolo de sempre (usuario falando
 * LONGE, alternando em ciclo, exigindo que o valor MUDE entre leituras).
 *
 * ARMADILHAS JA PAGAS, nao repetir:
 *  - Este comentario ja afirmou 0x50, 0xF0, 0x00 e 0x50 antes de chegar aqui.
 *    Todas as versoes erradas vieram de medir com o usuario perto da placa
 *    (media o microfone embutido) ou do medidor de pico congelado, que repete
 *    o ultimo valor quando o ADC nao entrega amostra nova.
 *  - Nao basta o numero ser MAIOR: tem que REAGIR a fonte certa. Foi o teste
 *    receptor ligado/desligado que resolveu, e ele so existiu porque o
 *    usuario questionou a recomendacao de dessoldar.
 *  - Este valor e DUPLICADO em ES8388_ADC_INPUT_DEFAULT (es8388.h), que
 *    audio_codec.c reaplica apos o boot. Mudar so um faz o firmware desfazer
 *    a mudanca sozinho -- ja aconteceu.
 *  - PGA 0x88 (+24dB) TRAVA o ADC entregando zeros, e so volta com reboot.
 *  - A causa de fundo do microfone mudo nao era nada disto: o I2S estava
 *    alocado como dois canais simplex em vez de um par full-duplex, ver
 *    i2s_init() em audio_codec.c. */
#define ES8388_ADC_INPUT_MIC1 0xF0

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(s_i2c_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8388_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
}

esp_err_t es8388_init(void)
{
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao inicializar I2C: %s", esp_err_to_name(err));
        return err;
    }

    int res = ESP_OK;
    res |= es8388_write_reg(ES8388_DACCONTROL3, 0x04); /* mute durante a configuração */
    res |= es8388_write_reg(ES8388_CONTROL2, 0x50);
    res |= es8388_write_reg(ES8388_CHIPPOWER, 0x00); /* liga o chip inteiro */

    /* Desabilita o DLL interno (recomendação do fabricante) */
    res |= es8388_write_reg(0x35, 0xA0);
    res |= es8388_write_reg(0x37, 0xD0);
    res |= es8388_write_reg(0x39, 0xD0);

    res |= es8388_write_reg(ES8388_MASTERMODE, 0x00); /* ES8388 em modo escravo de I2S (ESP32 é o master) */

    /* DAC */
    res |= es8388_write_reg(ES8388_DACPOWER, 0xC0); /* desliga DAC/Lout/Rout durante a config */
    res |= es8388_write_reg(ES8388_CONTROL1, 0x12);
    res |= es8388_write_reg(ES8388_DACCONTROL1, 0x18); /* I2S, 16 bits */
    res |= es8388_write_reg(ES8388_DACCONTROL2, 0x02); /* single speed, ratio 256 */
    res |= es8388_write_reg(ES8388_DACCONTROL16, 0x00);
    res |= es8388_write_reg(ES8388_DACCONTROL17, 0x90);
    res |= es8388_write_reg(ES8388_DACCONTROL20, 0x90);
    res |= es8388_write_reg(ES8388_DACCONTROL21, 0x80);
    res |= es8388_write_reg(ES8388_DACCONTROL23, 0x00);
    /* No ESP32-A1S (Audio Kit V2.2), LOUT1/ROUT1 vao para o amplificador de
     * alto-falante (SPOLN/SPORN) e LOUT2/ROUT2 vao para o jack EARPHONES
     * (HPOUTL/HPOUTR) — invertido em relacao ao mapeamento generico de
     * outras placas com ES8388. Zerar DACCONTROL26/27 (LOUT2/ROUT2) deixava
     * o EARPHONES praticamente mudo. Os quatro no mesmo nivel para os dois
     * caminhos funcionarem, independente de qual esteja fisicamente
     * conectado.
     *
     * Era 0x1E (0dB) -- relato do usuario 2026-08-21, apos trocar para um
     * amplificador externo real/mais potente: mesmo no volume minimo do
     * celular, o som sai alto. Reduzido para 0x14 (~-15dB em relacao ao
     * 0dB anterior, passo de ~1.5dB/contagem) para dar mais margem antes de
     * chegar no amplificador -- ajustar de novo se ainda nao for
     * suficiente (ou insuficiente demais) com o amplificador atual. */
    res |= es8388_write_reg(ES8388_DACCONTROL24, 0x14);
    res |= es8388_write_reg(ES8388_DACCONTROL25, 0x14);
    res |= es8388_write_reg(ES8388_DACCONTROL26, 0x14);
    res |= es8388_write_reg(ES8388_DACCONTROL27, 0x14);
    res |= es8388_write_reg(ES8388_DACPOWER, DAC_OUTPUT_ALL); /* liga DAC + Lout1/2 + Rout1/2 */

    /* ADC/microfone permanece desligado nesta fase do projeto. */
    res |= es8388_write_reg(ES8388_ADCPOWER, 0xFF);

    if (res != ESP_OK) {
        ESP_LOGE(TAG, "falha ao configurar registradores do ES8388");
        return ESP_FAIL;
    }

    /* Permanece mudo (ja mudo desde o "mute durante a configuracao" acima):
     * desmutar aqui deixava uma janela audivel entre o fim deste init e o
     * audio_codec_set_mute(true) em main.c, bem quando o I2S esta sendo
     * criado/habilitado (start do clock) -- causa classica de estalo no
     * boot. Quem desmuda de verdade e o bt_audio.c quando o audio A2DP
     * realmente comeca a tocar. */

    ESP_LOGI(TAG, "ES8388 inicializado (DAC/line-out, I2C SDA=%d SCL=%d)", PIN_I2C_SDA, PIN_I2C_SCL);
    return ESP_OK;
}

esp_err_t es8388_deinit(void)
{
    esp_err_t err = es8388_write_reg(ES8388_CHIPPOWER, 0xFF); /* reset e desliga o chip */
    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
    }
    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
    return err;
}

/* Uma unica tentativa de escrever os dois registradores de volume -- ver
 * es8388_set_volume() pra retry+log. */
static esp_err_t es8388_set_volume_once(uint8_t reg_val)
{
    esp_err_t err_l = es8388_write_reg(ES8388_DACCONTROL5, reg_val);
    esp_err_t err_r = es8388_write_reg(ES8388_DACCONTROL4, reg_val);
    return (err_l != ESP_OK) ? err_l : err_r;
}

esp_err_t es8388_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    /* 0-100 -> 0..-96dB em passos de 0.5dB (registrador usa contagem invertida) */
    uint8_t reg_val = (uint8_t)(2 * (100 - volume) * 96 / 100);

    /* Mesmo diagnostico 2026-08-25 do es8388_set_mute(): escrita por I2C sem
     * checar erro nem logar podia deixar o volume travado numa atenuacao
     * errada (ex.: quase mudo) sem nenhum rastro no log -- retry + log em
     * caso de falha, mesma logica do mute. */
    esp_err_t err = es8388_set_volume_once(reg_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao escrever volume (reg=0x%02x) via I2C (%s) -- tentando de novo", reg_val,
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5));
        err = es8388_set_volume_once(reg_val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "segunda tentativa de escrever volume tambem falhou (%s) -- volume pode ter ficado incorreto",
                     esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "segunda tentativa de escrever volume funcionou");
        }
    }
    return err;
}

esp_err_t es8388_get_volume(int *volume)
{
    /* Volume não é lido de volta do hardware nesta versão — quem chama deve
     * manter o último valor definido (ver storage.c / NVS_KEY_VOLUME_USER). */
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

/* Uma unica tentativa de mute/unmute -- extraido pra poder repetir em
 * es8388_set_mute() sem duplicar a logica de leitura+escrita. */
static esp_err_t es8388_set_mute_once(bool mute)
{
    uint8_t reg;
    esp_err_t err = i2c_master_transmit_receive(
        s_i2c_dev, (uint8_t[]){ES8388_DACCONTROL3}, 1, &reg, 1, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        return err;
    }
    reg = mute ? (reg | 0x04) : (reg & ~0x04);
    return es8388_write_reg(ES8388_DACCONTROL3, reg);
}

esp_err_t es8388_set_mute(bool mute)
{
    /* Diagnostico 2026-08-25: relato do usuario de audio ficando mudo "do
     * nada", com todo o resto do log (eventos de conexao/estado) parecendo
     * normal -- achado lendo o codigo: esta funcao podia falhar a
     * leitura/escrita por I2C (pico de barramento, contencao) e voltar erro
     * SILENCIOSO, que nenhum chamador (bt_audio.c/dlna_renderer.c) checava.
     * Ou seja, o registrador de mute podia nunca ser escrito de verdade sem
     * deixar rastro nenhum no log. Agora loga toda falha E tenta de novo uma
     * vez (a maioria de erros de I2C e transitoria) antes de desistir. */
    esp_err_t err = es8388_set_mute_once(mute);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao %s o DAC via I2C (%s) -- tentando de novo",
                 mute ? "mutar" : "desmutar", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5));
        err = es8388_set_mute_once(mute);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "segunda tentativa de %s tambem falhou (%s) -- estado do mute pode ter ficado incorreto",
                     mute ? "mutar" : "desmutar", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "segunda tentativa de %s funcionou", mute ? "mutar" : "desmutar");
        }
    }
    return err;
}

esp_err_t es8388_mic_init(void)
{
    /* SEQUENCIA OFICIAL DO FABRICANTE para gravacao com microfone diferencial.
     *
     * Copiada do "ES8388 User Guide", secao 8.4.1 ("Fully-Differential
     * Microphone input circuit and sample code"). O datasheet foi finalmente
     * lido em 2026-08-29 (pdftotext no PDF da Radxa) depois de o Celio sugerir
     * procurar material sobre o CODEC em vez da placa -- e ele traz coisas que
     * faltavam aqui e que nenhuma tentativa por palpite tinha acertado:
     *
     *  - Reg 0x02 = 0xF3 ANTES de configurar: para STM, DLL e bloco digital.
     *  - Reg 0x02 = 0x55 DEPOIS: religa DLL/STM/digital JA em modo gravacao.
     *    Configurar com o bloco digital rodando era o que faziamos, e e
     *    candidato direto pro ADC subir aleatoriamente saudavel/travado/chiando.
     *  - Reg 0x2B = 0x80: ADC e DAC compartilhando o MESMO LRCK. Nunca
     *    escreviamos isso, e o ADC e o DAC rodam no mesmo I2S aqui.
     *  - Reg 0x00 = 0x05 e Reg 0x01 = 0x40: "start up reference".
     *
     * Mantido diferente do exemplo de proposito:
     *  - Reg 0x0C = 0x0C (I2S 16 bits) em vez de 0x40: nosso canal I2S e de
     *    16 bits (ver i2s_init em audio_codec.c). O exemplo usa 24.
     *  - Reg 0x09 = 0x77 (+21dB) como o exemplo; 0x88 TRAVA o ADC nesta placa.
     */
    int res = ESP_OK;

    res |= es8388_write_reg(ES8388_CHIPPOWER, 0xF3); /* para STM, DLL e bloco digital */
    vTaskDelay(pdMS_TO_TICKS(10));

    res |= es8388_write_reg(ES8388_MASTERMODE, 0x00); /* I2S escravo */
    res |= es8388_write_reg(0x2B, 0x80);              /* ADC e DAC no mesmo LRCK */
    /* CONTROL1/CONTROL2 NAO sao tocados aqui de proposito. O exemplo do
     * datasheet manda 0x05/0x40, mas ele e de um aparelho que SO grava --
     * aqui o mesmo codec tambem reproduz, e es8388_init() ja deixou esses
     * registradores no que o DAC precisa (0x12/0x50). Sobrescrever quebrou o
     * caminho de reproducao: nem o beep de teste saia (2026-08-29). */
    res |= es8388_write_reg(ES8388_ADCPOWER, 0x00);   /* liga ADC e entradas LIN/RIN */

    res |= es8388_write_reg(ES8388_ADCCONTROL1, 0x77); /* PGA +21dB */
    res |= es8388_write_reg(ES8388_ADCCONTROL2, ES8388_ADC_INPUT_MIC1); /* 0xF0 diferencial */
    res |= es8388_write_reg(ES8388_ADCCONTROL3, 0x02); /* par diferencial LIN1/RIN1 */
    res |= es8388_write_reg(ES8388_ADCCONTROL4, 0x0C); /* I2S 16 bits (nosso caso) */
    res |= es8388_write_reg(ES8388_ADCCONTROL5, 0x02); /* MCLK/LRCK = 256 */

    /* ADCCONTROL6 (0x0E) -- polaridade do ADC e filtro passa-alta. Ficou no
     * padrao de fabrica (0x30) a sessao inteira; nem o exemplo do datasheet o
     * menciona. Achado ao varrer valores com o Celio cantando (2026-08-29),
     * comparando a RELACAO sinal/ruido e nao o volume bruto:
     *
     *   0x30 (padrao) -> voz  7504 / piso 122 =  61x
     *   0x10          -> voz 18953 / piso 126 = 150x   <- ESTE
     *   0x20          -> voz 20626 / piso 352 =  59x
     *   0x70          -> voz 21592 / piso 353 =  61x
     *
     * 0x20 e 0x70 davam mais volume, mas subiam o ruido junto. 0x10 da 2,5x
     * mais voz com o MESMO piso -- e o unico ajuste do dia que melhorou a
     * proporcao em vez do volume. Foi o que resolveu a distancia. */
    /* REVERTIDO pro padrao 0x30 em 2026-08-29: com 0x10 o ADC passou a subir
     * SATURADO (piso=32768) em 5 de 5 boots, contra 87% de saudaveis antes.
     * O ganho de relacao sinal/ruido que 0x10 dava (2,5x mais voz com o mesmo
     * piso, medido ao vivo) nao compensa o microfone nao subir. Se for tentar
     * de novo, testar a taxa de boot ANTES de gravar. */
    res |= es8388_write_reg(0x0E, 0x30);
    res |= es8388_write_reg(ES8388_ADCCONTROL8, 0x00); /* volume ADC esquerdo 0dB */
    res |= es8388_write_reg(ES8388_ADCCONTROL9, 0x00); /* volume ADC direito 0dB */

    /* ALC nos valores que o proprio datasheet recomenda para VOZ (tabela
     * 8.3.2): ganho maximo +23,5dB (o dobro do que vinhamos usando), alvo
     * -4,5dB, decaimento 820us, ataque 416us, e o noise gate do codec LIGADO
     * em -40,5dB com NGG=01 (muta a saida do ADC no silencio). Eu tinha
     * desligado esse noise gate por ter chutado o limiar e cortado a voz. */
    /* 0xFA = ALCSEL estereo, MAXGAIN=111 (+35,5dB, o maximo), MINGAIN=010.
     *
     * Escolha do Celio (2026-08-29) pelo ALCANCE. Ele dispensou a preocupacao
     * com microfonia ("se ocorrer eu cubro o mic com algo"), o que libera usar
     * o teto do datasheet.
     *
     * Passou a fazer sentido depois que o caminho de captura ficou limpo: com
     * a sequencia oficial de inicializacao e o hold gain, o sinal cru do ADC em
     * silencio caiu pra mediana 24 e pico 176 (antes: impulsos de ate 9169).
     * Antes disso, ganho alto so amplificava aquele lixo.
     *
     * O que NAO se resolve com ganho: o sinal do receptor chega fraco e o
     * caminho tem ruido proprio -- amplificar levanta voz e ruido na mesma
     * proporcao. Como o Celio observou, "so aumentou o volume do mesmo".
     * Melhorar de verdade exige sinal mais forte ENTRANDO na placa
     * (pre-amplificador externo). */
    res |= es8388_write_reg(ES8388_ADCCONTROL10, 0x00);
    /* 0x60 e nao 0xA0: ALCLVL e o NIVEL-ALVO que o ALC persegue. Em 0xA0 o
     * alvo fica perto do teto da escala, entao o ALC empurrava o sinal contra
     * o limite e cortava os picos -- o usuario ouvia a voz "roca". Baixar o
     * alvo resolveu a saturacao (de 12 amostras saturadas para 0). Reduzir o
     * ganho digital NAO adiantava: o ALC recompoe o nivel logo depois. */
    res |= es8388_write_reg(ES8388_ADCCONTROL11, 0xA0);
    res |= es8388_write_reg(ES8388_ADCCONTROL12, 0x12);
    res |= es8388_write_reg(ES8388_ADCCONTROL13, 0x06);
    /* 0xC5 e nao 0xC3: NGTH=11000 (-40,5dB) igual ao datasheet, mas NGG=10
     * ("hold gain") em vez de 01 ("mute ADC").
     *
     * Diferenca medida ao vivo (2026-08-29): com NGG=01 o gate MUTA a saida no
     * silencio, mas nao impede o ALC de continuar subindo o ganho procurando
     * sinal -- entao o ruido reaparecia amplificado assim que algo passava, e o
     * usuario ouvia "chiado que nao tem no inicio e vem depois", com oscilacao.
     * E o classico pumping. Com NGG=10 o ALC CONGELA o ganho enquanto nao ha
     * voz, e o chiado sumiu (confirmado no teste A/B/C: as tres opcoes de ganho
     * ficaram em silencio).
     *
     * A tabela 8.3.2 do datasheet lista os dois modos; peguei o primeiro sem
     * entender a distincao. */
    /* 0xFD: NGTH=11111 (limiar MAXIMO), NGG=10 (hold gain), NGAT=1.
     *
     * O limiar do datasheet (-40,5dB, 0xC5) era baixo demais e criava um
     * circulo vicioso: o ALC amplifica o ruido no silencio ate o alvo, o ruido
     * passa do limiar do gate, o gate nao trava, e o ALC continua subindo.
     * Medido: piso de 17790 com pico saturado em 32765, EM SILENCIO.
     *
     * Com o limiar no maximo o gate congela o ganho ANTES de o ruido crescer:
     * piso caiu para 799 (pico 1573), 22x menor. */
    res |= es8388_write_reg(ES8388_ADCCONTROL14, 0xC5);

    vTaskDelay(pdMS_TO_TICKS(10));
    /* 0x00 (tudo ligado), NAO o 0x55 do exemplo: 0x55 religa apenas os blocos
     * de GRAVACAO, e aqui o DAC tem de voltar junto -- com 0x55 o aparelho
     * ficava totalmente mudo, nem o beep saia. */
    res |= es8388_write_reg(ES8388_CHIPPOWER, 0x00); /* religa DLL/STM/digital, ADC e DAC */
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "ES8388 ADC/microfone habilitado (sequencia do datasheet, entrada 0x%02X)",
             ES8388_ADC_INPUT_MIC1);
    return res == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t es8388_mic_deinit(void)
{
    return es8388_write_reg(ES8388_ADCPOWER, 0xFF);
}

/* Diagnostico 2026-08-28: o pico captado ficou preso no piso de ruido
 * (~40-70 de 32767) mesmo com o usuario falando alto, o que indica que o
 * ADC nao esta ouvindo o microfone de verdade -- provavelmente a ENTRADA
 * selecionada esta errada pra esta placa. Em vez de recompilar a cada
 * palpite, isto permite varrer as opcoes ao vivo (ver POST /api/mic/input).
 *
 * ADCCONTROL2: bits 7:6 = entrada da esquerda, 5:4 = da direita.
 * 0x00 = LINPUT1/RINPUT1 | 0x50 = LINPUT2/RINPUT2 | 0xF0 = diferencial. */
esp_err_t es8388_mic_set_input(uint8_t adccontrol2)
{
    return es8388_write_reg(ES8388_ADCCONTROL2, adccontrol2);
}

/* Escrita crua de registrador -- so diagnostico ao vivo (POST /api/mic/reg).
 * Serve pra testar hipoteses de configuracao do codec sem um ciclo de
 * compilar/gravar por palpite, que e lento demais quando ha muitas
 * combinacoes possiveis. */
esp_err_t es8388_write_reg_raw(uint8_t reg, uint8_t val)
{
    return es8388_write_reg(reg, val);
}

esp_err_t es8388_read_reg_raw(uint8_t reg, uint8_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(s_i2c_dev, &reg, 1, out, 1, pdMS_TO_TICKS(100));
}

esp_err_t es8388_mic_set_gain(int gain_0_to_100)
{
    if (gain_0_to_100 < 0) {
        gain_0_to_100 = 0;
    } else if (gain_0_to_100 > 100) {
        gain_0_to_100 = 100;
    }
    uint8_t step = (uint8_t)((gain_0_to_100 * 7) / 100); /* 0-7, ~3dB por passo, 0 a +24dB */
    uint8_t reg_val = (uint8_t)((step << 4) | step); /* mesmo ganho L/R */
    return es8388_write_reg(ES8388_ADCCONTROL1, reg_val);
}
