/* outputs.c - LEDs and SG90 servo.
 *
 * Servo: a 50 Hz signal (20 ms period). The pulse width sets the angle:
 * 0.5 ms = 0 deg, 1.5 ms = 90 deg, 2.5 ms = 180 deg. With a 14-bit timer, 20 ms = 16384 ticks. */
#include "outputs.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "system_fsm.h"

#define SERVO_MODE LEDC_LOW_SPEED_MODE
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_CHANNEL LEDC_CHANNEL_0
#define SERVO_FREQ_HZ 50
#define SERVO_RES LEDC_TIMER_14_BIT
#define SERVO_PERIOD_US 20000u
#define SERVO_MIN_US 500u
#define SERVO_MAX_US 2500u

static const char *TAG = "outputs";
static uint8_t s_bits = 0xFF;   /* forces the first outputs_apply() to set everything */

static uint32_t servo_duty(int deg)
{
    const uint32_t pulse_us = SERVO_MIN_US + (uint32_t)deg * (SERVO_MAX_US - SERVO_MIN_US) / 180u;
    return pulse_us * (1u << SERVO_RES) / SERVO_PERIOD_US;
}

esp_err_t outputs_init(void)
{
    const gpio_config_t led = {
        .pin_bit_mask = (1ULL << PIN_LED1) | (1ULL << PIN_LED2) | (1ULL << PIN_LED3),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&led), TAG, "LED pins");

    const ledc_timer_config_t timer = {
        .speed_mode = SERVO_MODE,
        .duty_resolution = SERVO_RES,
        .timer_num = SERVO_TIMER,
        .freq_hz = SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LEDC timer");

    const ledc_channel_config_t ch = {
        .gpio_num = PIN_SERVO,
        .speed_mode = SERVO_MODE,
        .channel = SERVO_CHANNEL,
        .timer_sel = SERVO_TIMER,
        .duty = servo_duty(SERVO_CLOSED_DEG),
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "LEDC channel");

    outputs_apply(0);
    return ESP_OK;
}

void outputs_apply(uint8_t bits)
{
    gpio_set_level(PIN_LED1, (bits & OUT_LED1) ? 1 : 0);
    gpio_set_level(PIN_LED2, (bits & OUT_LED2) ? 1 : 0);
    gpio_set_level(PIN_LED3, (bits & OUT_LED3) ? 1 : 0);

    if ((bits ^ s_bits) & OUT_DOOR) {
        const int deg = (bits & OUT_DOOR) ? SERVO_OPEN_DEG : SERVO_CLOSED_DEG;
        ledc_set_duty(SERVO_MODE, SERVO_CHANNEL, servo_duty(deg));
        ledc_update_duty(SERVO_MODE, SERVO_CHANNEL);
    }
    s_bits = bits;
}
