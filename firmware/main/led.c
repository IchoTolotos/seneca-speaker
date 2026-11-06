#include "led.h"

#include <stdatomic.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

static atomic_int s_mode = SC_LED_OFF;

static void led_write(bool on)
{
#if CONFIG_GLS_LED_ACTIVE_LOW
    gpio_set_level(CONFIG_GLS_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(CONFIG_GLS_LED_GPIO, on ? 1 : 0);
#endif
}

/* 100 ms steps; pattern bit i = LED on in step i of a 2 s cycle (20 steps). */
static uint32_t pattern(sc_led_t m)
{
    switch (m) {
    case SC_LED_IDLE:        return 0x00001;  /* short blip every 2 s */
    case SC_LED_LISTENING:   return 0xFFFFF;  /* solid */
    case SC_LED_THINKING:    return 0x33333;  /* fast blink */
    case SC_LED_SPEAKING:    return 0xFFFF0;  /* mostly on, short gap */
    case SC_LED_CONNECTING:  return 0x003FF;  /* 1 Hz */
    case SC_LED_ERROR:       return 0x55555;  /* very fast */
    case SC_LED_NEEDS_LOGIN: return 0x00005;  /* double blip */
    case SC_LED_MUTED:       return 0x00000;  /* off */
    default:                 return 0;
    }
}

static void led_task(void *arg)
{
    int step = 0;
    for (;;) {
        uint32_t p = pattern((sc_led_t)atomic_load(&s_mode));
        led_write((p >> step) & 1);
        step = (step + 1) % 20;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void gls_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_GLS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    led_write(false);
    xTaskCreatePinnedToCoreWithCaps(led_task, "led", 2048, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void gls_led_set(sc_led_t mode)
{
    atomic_store(&s_mode, (int)mode);
}
