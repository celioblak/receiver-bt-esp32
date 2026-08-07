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

static void relay_set(bool on)
{
    gpio_set_level(PIN_RELAY_CONTROL, on ? 1 : 0);
    s_relay_on = on;
    logger_log(ESP_LOG_INFO, TAG, "Amplificador %s", on ? "ligado" : "desligado");
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

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_RELAY_CONTROL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_RELAY_CONTROL, 0); /* começa desligado */

    const esp_timer_create_args_t timer_args = {
        .callback = off_timer_cb,
        .name = "relay_off",
    };
    esp_timer_create(&timer_args, &s_off_timer);

    logger_log(ESP_LOG_INFO, TAG, "Controle do rele pronto (GPIO%d)", PIN_RELAY_CONTROL);
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
