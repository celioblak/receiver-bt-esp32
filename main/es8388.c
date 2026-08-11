#include "es8388.h"

#include "config.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

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
     * o EARPHONES praticamente mudo. Os quatro em 0dB para os dois caminhos
     * funcionarem, independente de qual esteja fisicamente conectado. */
    res |= es8388_write_reg(ES8388_DACCONTROL24, 0x1E); /* 0dB */
    res |= es8388_write_reg(ES8388_DACCONTROL25, 0x1E);
    res |= es8388_write_reg(ES8388_DACCONTROL26, 0x1E);
    res |= es8388_write_reg(ES8388_DACCONTROL27, 0x1E);
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

esp_err_t es8388_set_volume(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    /* 0-100 -> 0..-96dB em passos de 0.5dB (registrador usa contagem invertida) */
    uint8_t reg_val = (uint8_t)(2 * (100 - volume) * 96 / 100);
    esp_err_t err = es8388_write_reg(ES8388_DACCONTROL5, reg_val);
    err |= es8388_write_reg(ES8388_DACCONTROL4, reg_val);
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

esp_err_t es8388_set_mute(bool mute)
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
