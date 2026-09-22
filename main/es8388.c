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

/* HISTORICO DA ESCOLHA DE ENTRADA -- leia antes de mexer em 0x0A/0x0B.
 *
 * Este driver passou a sessao inteira em 0x0A=0xF0 / 0x0B=0x02, o modo
 * "fully-differential LIN1-RIN1", escolhido por ser o recomendado pelo
 * datasheet para microfone e por ter dado o maior numero nas medicoes.
 *
 * ERA A ENTRADA ERRADA, e o numero maior nao provava nada. O esquematico do
 * modulo ESP32-A1S mostra que LIN1 e RIN1 sao os DOIS MICROFONES EMBUTIDOS
 * da placa (MIC1P no pino 17, MIC2P no 15), e que a entrada de linha do jack
 * (LINEINL/LINEINR, pinos 22 e 21) chega em LIN2/RIN2. Ver a tabela completa
 * em es8388_mic_input_t (es8388.h).
 *
 * Logo, o modo diferencial LIN1-RIN1 estava amplificando (mic embutido
 * esquerdo) menos (mic embutido direito). Os dois estao a poucos centimetros
 * um do outro e captam praticamente o mesmo som: a voz, que chega igual nos
 * dois, e quase toda CANCELADA na subtracao, enquanto o ruido proprio de cada
 * microfone, descorrelacionado, passa inteiro. E o arranjo de pior relacao
 * sinal/ruido que esta placa permite -- e explica exatamente os dois sintomas
 * que sobreviveram a todos os ajustes de ganho, PGA e ALC:
 *
 *   - voz baixa, obrigando a cantar colado no microfone;
 *   - chiado alto, que so piorava quando se aumentava o ganho.
 *
 * Tambem explica as observacoes que nao fechavam com "o receptor sem fio e a
 * fonte": dessoldar os microfones embutidos derrubou a captacao (eles ERAM a
 * fonte), e havia microfonia mesmo com o microfone do usuario desligado (os
 * embutidos ouvem o alto-falante).
 *
 * NAO REMOVER OS MICROFONES EMBUTIDOS DA PLACA. Eles nao captam nada util
 * nesta entrada (medido: o Celio falando alto a 10cm nao move o nivel), entao
 * parece tentador tirar os dois -- e foi o que eu cheguei a recomendar. Mas
 * SEM microfone na placa o ADC NAO SOBE: com resistor de 1k e de 10k no lugar
 * do MIC1 foram 0 boots saudaveis em 18 tentativas (2026-08-29), e os boots so
 * voltaram quando ele recolocou o MIC1. Captar e inicializar sao coisas
 * diferentes; a segunda ja esta medida, e o resultado e ruim.
 *
 * ARMADILHAS JA PAGAS, nao repetir:
 *  - Nao basta o numero ser MAIOR: tem que REAGIR a fonte certa, medido com o
 *    usuario LONGE da placa. Medir com ele perto mede o microfone embutido, e
 *    foi essa a origem de tres trocas de entrada erradas -- inclusive da
 *    conclusao de que 0x50 "nao sai do piso", que na verdade era o modo certo
 *    ficando quieto porque a fonte medida estava do lado errado.
 *  - PGA 0x88 (+24dB) TRAVA o ADC entregando zeros, e so volta com reboot.
 *  - A causa do microfone MUDO (anterior a tudo isto) nao era entrada nenhuma:
 *    o I2S estava alocado como dois canais simplex em vez de um par
 *    full-duplex, ver i2s_init() em audio_codec.c.
 *  - O valor ja foi duplicado entre .c e .h e os dois divergiram, fazendo o
 *    firmware desfazer a propria mudanca depois do boot. Agora existe um
 *    lugar so: s_input_mode, atras de es8388_mic_set_input_mode(). */

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(s_i2c_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t i2c_bus_init(void)
{
    if (s_i2c_dev != NULL) {
        return ESP_OK; /* ja inicializado -- ver es8388_early_mute() */
    }
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

/* Cala o codec ANTES de o clock I2S existir.
 *
 * audio_codec_init() sobe o I2S primeiro e so ~50ms depois configura o codec
 * (ordem deliberada: o codec precisa de MCLK presente). O efeito colateral e
 * uma janela em que o ES8388 esta no estado de RESET DE FABRICA -- DAC
 * desmutado, saidas ligadas -- ja recebendo dados do ESP32 pelo barramento.
 * E um estalo por construcao, e sobrou como o "estalo ao reiniciar" depois de
 * as outras tres causas serem corrigidas.
 *
 * Isto abre so o I2C e cala o caminho de saida; a configuracao de verdade
 * continua em es8388_init(), depois do MCLK. */
esp_err_t es8388_early_mute(void)
{
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    int res = ESP_OK;
    res |= es8388_write_reg(ES8388_DACCONTROL3, 0x04); /* muta o DAC */
    res |= es8388_write_reg(ES8388_DACPOWER, 0xC0);    /* desliga DAC e saidas */
    return res == ESP_OK ? ESP_OK : ESP_FAIL;
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
    /* PARA O BLOCO DIGITAL ANTES DE CONFIGURAR (secao 10.3 do User Guide,
     * "start up play back mode": Reg 0x02 = 0xF3 logo no inicio, e so no fim
     * religar).
     *
     * Este init escrevia 0x02 = 0x00 AQUI, ou seja, ligava DLL, maquina de
     * estados e bloco digital e so entao mexia em formato, clock, mixer e
     * saidas -- configurando tudo com o caminho de audio em funcionamento.
     * E exatamente o mesmo defeito que existia no ADC e cuja correcao levou os
     * boots saudaveis de ~35% para ~87% (ver es8388_mic_config_begin). Cada
     * escrita de formato/clock com o bloco rodando e um degrau no caminho
     * analogico, e degrau e estalo. */
    res |= es8388_write_reg(ES8388_CHIPPOWER, 0xF3); /* para STM, DLL e bloco digital */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Desabilita o DLL interno (recomendação do fabricante) */
    res |= es8388_write_reg(0x35, 0xA0);
    res |= es8388_write_reg(0x37, 0xD0);
    res |= es8388_write_reg(0x39, 0xD0);

    res |= es8388_write_reg(ES8388_MASTERMODE, 0x00); /* ES8388 em modo escravo de I2S (ESP32 é o master) */
    /* ADC e DAC no MESMO LRCK. A secao 10.1 do User Guide ("start up
     * codec") pede isto na subida do codec, e nao so quando o ADC entra --
     * este driver so escrevia dentro de es8388_mic_config_begin(), entao com
     * o microfone desligado o registrador ficava no default. Nao muda nada
     * enquanto so o DAC roda; escrito aqui por seguir a sequencia oficial. */
    res |= es8388_write_reg(0x2B, 0x80);

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
    /* ESTALO RESIDUAL NO BOOT: e AQUI, e e FISICO -- nao perseguir por
     * software de novo.
     *
     * Ligar os drivers de saida carrega os capacitores de acoplamento, e essa
     * corrente inicial e um transiente audivel. Em 2026-08-30 foram achadas e
     * corrigidas QUATRO causas de software do estalo (bloco digital rodando
     * durante a configuracao; o CHIPPOWER caindo na subida do microfone com o
     * DAC desmutado; o DAC ficando desmutado em repouso; e o codec no reset de
     * fabrica com o clock ja correndo). Depois das quatro, o Celio confirmou:
     * o estalo diminuiu mas continua, e "pelo volume e rapidez nao vejo
     * problema".
     *
     * Se for atras disso um dia, o caminho e hardware (rampa de VMID, ordem de
     * energizacao dos trilhos analogicos, mute externo na saida), nao
     * registrador. */
    res |= es8388_write_reg(ES8388_DACPOWER, DAC_OUTPUT_ALL); /* liga DAC + Lout1/2 + Rout1/2 */

    /* ADC/microfone permanece desligado nesta fase do projeto. */
    res |= es8388_write_reg(ES8388_ADCPOWER, 0xFF);

    /* Religa DLL, maquina de estados e bloco digital, agora com tudo ja
     * configurado -- o fim da sequencia da secao 10.3. O DAC continua MUDO
     * (0x19 = 0x04, escrito la em cima), entao este religar nao produz som
     * nenhum: quem desmuta e o bt_audio.c/dlna_renderer.c quando o audio de
     * verdade comeca. */
    vTaskDelay(pdMS_TO_TICKS(10));
    res |= es8388_write_reg(ES8388_CHIPPOWER, 0x00);
    vTaskDelay(pdMS_TO_TICKS(50));

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

/* Registrador 15 (0x0F): ADCMute, soft ramp e ADCLeR. A secao 10.2 do User
 * Guide manda escrever 0x0F = 0x30 ("UnMute ADC") na subida da gravacao, e
 * este driver nunca escrevia nada aqui -- o registrador ficava no que o
 * reset de fabrica deixasse. */
#define ES8388_ADCCONTROL7  0x0F

/* Entrada escolhida, memorizada entre reconfiguracoes do ADC. Comeca no modo
 * que le o JACK DE ENTRADA (LINPUT2/RINPUT2), que e onde a entrada de linha
 * desta placa esta ligada -- ver es8388_mic_input_t em es8388.h. */
static es8388_mic_input_t s_input_mode = ES8388_IN_LIN2_SE;

/* Guarda se o DAC ja estava mudo quando o ADC comecou a ser (re)configurado,
 * para nao desmutar por engano algo que o resto do firmware queria mudo. */
static bool s_dac_estava_mudo = true;

/* Valores de 0x0A (selecao de entrada) e 0x0B (qual par diferencial) para
 * cada modo, na ordem do enum. */
static const uint8_t s_input_regs[ES8388_IN_COUNT][2] = {
    [ES8388_IN_LIN1_SE]   = {0x00, 0x00},
    [ES8388_IN_LIN2_SE]   = {0x50, 0x00},
    [ES8388_IN_DIFF_MIC1] = {0xF0, 0x02},
    [ES8388_IN_DIFF_MIC2] = {0xF0, 0x82},
    [ES8388_IN_LIN2_SE_DIR] = {0x50, 0x00},
};

static const char *const s_input_nomes[ES8388_IN_COUNT] = {
    [ES8388_IN_LIN1_SE]   = "mic_embutido",
    [ES8388_IN_LIN2_SE]   = "jack_entrada",
    [ES8388_IN_DIFF_MIC1] = "diferencial_mic",
    [ES8388_IN_DIFF_MIC2] = "diferencial_jack",
    [ES8388_IN_LIN2_SE_DIR] = "jack_canal_direito",
};

bool es8388_mic_input_usa_canal_direito(void)
{
    return s_input_mode == ES8388_IN_LIN2_SE_DIR;
}

const char *es8388_mic_input_name(es8388_mic_input_t modo)
{
    if ((int)modo < 0 || modo >= ES8388_IN_COUNT) {
        return "?";
    }
    return s_input_nomes[modo];
}

es8388_mic_input_t es8388_mic_get_input_mode(void)
{
    return s_input_mode;
}

esp_err_t es8388_mic_set_input_mode(es8388_mic_input_t modo)
{
    if ((int)modo < 0 || modo >= ES8388_IN_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_input_mode = modo;
    int res = ESP_OK;
    res |= es8388_write_reg(ES8388_ADCCONTROL2, s_input_regs[modo][0]);
    res |= es8388_write_reg(ES8388_ADCCONTROL3, s_input_regs[modo][1]);
    ESP_LOGI(TAG, "entrada do ADC: %s (0x0A=0x%02X 0x0B=0x%02X)", s_input_nomes[modo],
             s_input_regs[modo][0], s_input_regs[modo][1]);
    return res == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t es8388_mic_config_begin(void)
{
    /* SEQUENCIA OFICIAL DO FABRICANTE, secoes 8.4.1 e 10.1 do "ES8388 User
     * Guide". O datasheet foi lido em 2026-08-29 (pdftotext no PDF da Radxa)
     * depois de o Celio sugerir procurar material sobre o CODEC em vez da
     * placa -- e trouxe o que nenhuma tentativa por palpite tinha acertado.
     *
     * ESTA METADE PRESSUPOE O CLOCK I2S PARADO, e e por isso que ela existe
     * separada de es8388_mic_config_end(). Causa raiz do ADC subir
     * aleatoriamente travado ou chiando, secao 5.1 do mesmo guia:
     *
     *   "In slave mode, ES8388 can auto check MCLK/LRCK ratio and MCLK/SCLK
     *    ratio."
     *
     * O codec MEDE os clocks sozinho quando a DLL e o bloco digital acordam
     * (reg 0x02). Ate agora esse despertar acontecia com o I2S ja correndo ha
     * 12 segundos, ou seja, no meio de um frame qualquer -- e a auto-deteccao
     * pegava o que estivesse passando. Dai o resultado ser indeterminado boot
     * a boot: acertou, ADC saudavel; errou o alinhamento, o ADC entrega bits
     * deslocados (o "chiado") ou nada (o "travado"). Nenhum registrador
     * reescrito depois recupera, porque o erro nao esta na configuracao -- e
     * o que explica as sete tentativas de recuperacao em runtime que
     * falharam, inclusive resetar a maquina de estados e o chip inteiro.
     *
     * Agora o clock so nasce DEPOIS que tudo aqui esta escrito, e o bloco
     * digital so acorda em es8388_mic_config_end(), com o clock ja estavel.
     *
     * Mantido diferente do exemplo do datasheet de proposito:
     *  - Reg 0x0C = 0x0C (I2S 16 bits) em vez de 0x40: nosso canal I2S e de
     *    16 bits (ver i2s_init em audio_codec.c). O exemplo usa 24.
     *  - CONTROL1/CONTROL2 (0x00/0x01) NAO sao tocados. O exemplo manda
     *    0x05/0x40, mas ele e de um aparelho que SO grava -- aqui o mesmo
     *    codec tambem reproduz, e es8388_init() ja os deixou no que o DAC
     *    precisa (0x12/0x50). Sobrescrever quebrou a reproducao: nem o beep
     *    de teste saia (2026-08-29).
     *  - Reg 0x02 = 0x00 no fim, nao o 0x55 do exemplo, pelo mesmo motivo:
     *    0x55 religa so os blocos de GRAVACAO e o aparelho fica mudo. */
    int res = ESP_OK;

    /* MUTA O DAC ANTES de derrubar o bloco digital.
     *
     * Esta funcao para e religa o CHIPPOWER, e isso passa pelo caminho de
     * reproducao inteiro. Rodando ~12s depois de cada boot (e a cada corte de
     * MCLK), com o DAC desmutado e as saidas ligadas, o degrau vira um estalo
     * audivel -- que e o "chiado ao reiniciar o dispositivo" relatado pelo
     * Celio em 2026-08-30. O estado anterior do mute e restaurado em
     * es8388_mic_config_end(). */
    s_dac_estava_mudo = true;
    uint8_t dac3 = 0;
    if (es8388_read_reg_raw(ES8388_DACCONTROL3, &dac3) == ESP_OK) {
        s_dac_estava_mudo = (dac3 & 0x04) != 0;
    }
    res |= es8388_write_reg(ES8388_DACCONTROL3, dac3 | 0x04);

    res |= es8388_write_reg(ES8388_CHIPPOWER, 0xF3); /* para STM, DLL e bloco digital */
    vTaskDelay(pdMS_TO_TICKS(10));

    res |= es8388_write_reg(ES8388_MASTERMODE, 0x00); /* I2S escravo */
    res |= es8388_write_reg(0x2B, 0x80);              /* ADC e DAC no mesmo LRCK */
    res |= es8388_write_reg(ES8388_ADCPOWER, 0x00);   /* liga ADC e entradas LIN/RIN */

    /* PGA +9dB, nao mais +21dB. MEDIDO com o Celio cantando (2026-09-22):
     *
     *   PGA    mediana   pico   fator de crista   graves/agudos
     *   +21dB   16.952  29.106      1,7:1            1232x
     *   +15dB   10.987  28.892      2,6:1
     *   +9dB     3.540  12.465      3,5:1   <- este
     *   +3dB       584   2.887      4,9:1
     *    0dB       860   2.345      2,7:1             258x
     *
     * Fator de crista de 1,7:1 e sinal ESMAGADO -- voz natural fica entre 4:1
     * e 10:1 -- e nenhuma amostra batia em 32767, ou seja nao era clipe
     * digital: era SATURACAO ANALOGICA no proprio PGA, antes do conversor. Som
     * abafado e sem presenca e exatamente o que isso produz, e foi a queixa do
     * Celio ("parece caixa antiga").
     *
     * O 0x77 nao era um erro de calculo: foi escolhido quando o firmware lia a
     * ENTRADA ERRADA (os microfones embutidos subtraidos), onde o sinal era
     * minusculo e +21dB ainda era pouco. Com a entrada correta o sinal ficou
     * 130x maior e ninguem revisitou o ganho -- mesmo padrao do passa-baixa em
     * MIC_LPF_K: parametro certo para uma condicao que deixou de existir.
     *
     * +9dB equilibra crista (3,5:1) e nivel utilizavel. O volume que falta vem
     * do ganho DIGITAL (audio_codec_set_mic_gain), onde nao ha saturacao
     * analogica. Se a fonte mudar, e este o numero a revisar -- medindo o
     * fator de crista, nao o volume. */
    res |= es8388_write_reg(ES8388_ADCCONTROL1, 0x33); /* PGA +9dB (0x88 TRAVA o ADC) */
    res |= es8388_mic_set_input_mode(s_input_mode);    /* escreve 0x0A e 0x0B */
    res |= es8388_write_reg(ES8388_ADCCONTROL4, 0x0C); /* I2S 16 bits (nosso caso) */
    res |= es8388_write_reg(ES8388_ADCCONTROL5, 0x02); /* MCLK/LRCK = 256 */

    /* ADCCONTROL6 (0x0E) -- polaridade e filtro passa-alta do ADC. Fica no
     * padrao de fabrica 0x30, que e o que o proprio guia recomenda ("The
     * default setting are recommended to ADC_HPF_L and ADC_HPF_R", secao
     * 8.2). Ja foi tentado 0x10 aqui: dava mais sinal, mas o ADC passava a
     * subir SATURADO em 5 de 5 boots. Se voltar a esse teste, medir a taxa de
     * boot ANTES de gravar. */
    res |= es8388_write_reg(0x0E, 0x30);
    res |= es8388_write_reg(ES8388_ADCCONTROL7, 0x30); /* UnMute ADC (secao 10.2) */
    res |= es8388_write_reg(ES8388_ADCCONTROL8, 0x00); /* volume ADC esquerdo 0dB */
    res |= es8388_write_reg(ES8388_ADCCONTROL9, 0x00); /* volume ADC direito 0dB */

    /* ALC DESLIGADO (ALCSEL=00).
     *
     * O datasheet recomenda ALC para microfone, e a tabela 8.3.2 traz um
     * conjunto pronto para voz (0x12=0xE2, 0x13=0xA0, 0x14=0x12, 0x15=0x06,
     * 0x16=0xC3). Ele foi usado aqui e desligado depois de o Celio ouvir
     * chiado que crescia com o tempo: com a entrada errada o sinal util era
     * minusculo, entao o ALC subia o ganho ate o teto procurando voz e o que
     * ele encontrava era ruido.
     *
     * Continua desligado NESTE PASSO de proposito -- a entrada acabou de
     * mudar e o nivel que chega ao ADC e outro. Ligar o ALC junto misturaria
     * duas variaveis. Com a entrada certa o ALC passa a fazer sentido (e a
     * ter sinal de verdade para trabalhar); e o proximo ajuste a medir. */
    res |= es8388_write_reg(ES8388_ADCCONTROL10, 0x00);
    res |= es8388_write_reg(ES8388_ADCCONTROL11, 0xA0);
    res |= es8388_write_reg(ES8388_ADCCONTROL12, 0x12);
    res |= es8388_write_reg(ES8388_ADCCONTROL13, 0x06);
    /* NGG=10 ("hold gain") e nao 01 ("mute ADC"): com mute o gate silencia a
     * saida no silencio, mas nao impede o ALC de continuar subindo o ganho
     * procurando sinal -- o ruido reaparecia amplificado assim que algo
     * passava. E o pumping classico. Medido ao vivo em 2026-08-29. */
    res |= es8388_write_reg(ES8388_ADCCONTROL14, 0xC5);

    return res == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t es8388_mic_config_end(void)
{
    /* Acorda DLL, maquina de estados e bloco digital com o clock I2S ja
     * correndo e estavel -- e aqui que o codec faz a auto-deteccao de
     * MCLK/LRCK descrita em es8388_mic_config_begin(). */
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_err_t err = es8388_write_reg(ES8388_CHIPPOWER, 0x00);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Devolve o mute ao estado em que estava antes -- so desmuta se ja estava
     * desmutado. Espera o transiente do religar passar antes, senao o mute
     * teria sido inutil. */
    if (!s_dac_estava_mudo) {
        uint8_t dac3 = 0;
        if (es8388_read_reg_raw(ES8388_DACCONTROL3, &dac3) == ESP_OK) {
            es8388_write_reg(ES8388_DACCONTROL3, dac3 & (uint8_t)~0x04);
        }
    }
    ESP_LOGI(TAG, "ES8388 ADC/microfone habilitado (entrada %s)", es8388_mic_input_name(s_input_mode));
    return err;
}

esp_err_t es8388_mic_init(void)
{
    /* Versao em um passo so, para quem chamar com o clock ja correndo. O
     * caminho bom e i2s_init(), que chama as duas metades separadamente com
     * o enable do I2S no meio. */
    esp_err_t err = es8388_mic_config_begin();
    if (err != ESP_OK) {
        return err;
    }
    return es8388_mic_config_end();
}

esp_err_t es8388_mic_deinit(void)
{
    return es8388_write_reg(ES8388_ADCPOWER, 0xFF);
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
