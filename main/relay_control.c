#include "relay_control.h"

#include "config.h"
#include "logger.h"
#include "storage.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "relay_control";

static esp_timer_handle_t s_off_timer = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_relay_on = false;

/* true = o módulo aciona em nível BAIXO. Ver DEFAULT_RELAY_ACTIVE_LOW. */
static bool s_active_low = (DEFAULT_RELAY_ACTIVE_LOW != 0);
/* true = pino em dreno aberto. Ver DEFAULT_RELAY_OPEN_DRAIN. */
static bool s_open_drain = (DEFAULT_RELAY_OPEN_DRAIN != 0);

static void relay_configurar_pino(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_RELAY_CONTROL,
        .mode = s_open_drain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
        /* Sem pull-up interno de propósito: ele puxaria para 3,3V, que é
         * justamente a tensão que o módulo de 5V não reconhece como nível
         * alto. Quem tem de puxar é o pull-up do próprio módulo. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

void relay_control_set_open_drain(bool open_drain)
{
    s_open_drain = open_drain;
    storage_set_i32(NVS_KEY_RELAY_OPEN_DRAIN, open_drain ? 1 : 0);
    relay_configurar_pino();
    gpio_set_level(PIN_RELAY_CONTROL, (s_relay_on != s_active_low) ? 1 : 0);
    logger_log(ESP_LOG_INFO, TAG, "Pino do rele: %s",
               open_drain ? "dreno aberto" : "saida normal");
}

bool relay_control_get_open_drain(void)
{
    return s_open_drain;
}

static void relay_aplicar_nivel(bool on)
{
    gpio_set_level(PIN_RELAY_CONTROL, (on != s_active_low) ? 1 : 0);
}

static void relay_set(bool on)
{
    relay_aplicar_nivel(on);
    s_relay_on = on;
    logger_log(ESP_LOG_INFO, TAG, "Amplificador %s (nivel %d, active_low=%d)",
               on ? "ligado" : "desligado", (on != s_active_low) ? 1 : 0, (int)s_active_low);
}

void relay_control_set_active_low(bool active_low)
{
    s_active_low = active_low;
    storage_set_i32(NVS_KEY_RELAY_ACTIVE_LOW, active_low ? 1 : 0);
    relay_aplicar_nivel(s_relay_on); /* reaplica na hora, sem esperar evento */
    logger_log(ESP_LOG_INFO, TAG, "Polaridade do rele: aciona em nivel %s",
               active_low ? "BAIXO" : "ALTO");
}

bool relay_control_get_active_low(void)
{
    return s_active_low;
}

void relay_control_force_on(void)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        esp_timer_stop(s_off_timer);
        relay_set(true);
        xSemaphoreGive(s_mutex);
    }
}

static void off_timer_cb(void *arg)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        relay_set(false);
        xSemaphoreGive(s_mutex);
    }
}

void relay_control_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    int32_t od = DEFAULT_RELAY_OPEN_DRAIN;
    storage_get_i32(NVS_KEY_RELAY_OPEN_DRAIN, &od, DEFAULT_RELAY_OPEN_DRAIN);
    s_open_drain = (od != 0);
    relay_configurar_pino();

    int32_t act_low = DEFAULT_RELAY_ACTIVE_LOW;
    storage_get_i32(NVS_KEY_RELAY_ACTIVE_LOW, &act_low, DEFAULT_RELAY_ACTIVE_LOW);
    s_active_low = (act_low != 0);

    s_relay_on = false;
    relay_aplicar_nivel(false); /* começa desligado, na polaridade certa */

    const esp_timer_create_args_t timer_args = {
        .callback = off_timer_cb,
        .name = "relay_off",
    };
    esp_timer_create(&timer_args, &s_off_timer);

    logger_log(ESP_LOG_INFO, TAG, "Controle do rele pronto (GPIO%d, aciona em nivel %s)",
               PIN_RELAY_CONTROL, s_active_low ? "BAIXO" : "ALTO");
}

void relay_control_notify_playing(bool playing)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    esp_timer_stop(s_off_timer); /* ESP_ERR_INVALID_STATE se já parado — inofensivo */

    if (playing) {
        if (!s_relay_on) {
            relay_set(true);
        }
    } else {
        int32_t timeout_s = DEFAULT_RELAY_TIMEOUT_S;
        storage_get_i32(NVS_KEY_RELAY_TIMEOUT, &timeout_s, DEFAULT_RELAY_TIMEOUT_S);
        esp_timer_start_once(s_off_timer, (uint64_t)timeout_s * 1000000ULL);
    }

    xSemaphoreGive(s_mutex);
}

void relay_control_force_off(void)
{
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    esp_timer_stop(s_off_timer); /* ESP_ERR_INVALID_STATE se já parado — inofensivo */
    if (s_relay_on) {
        relay_set(false);
    }
    xSemaphoreGive(s_mutex);
}

bool relay_control_is_on(void)
{
    return s_relay_on;
}
