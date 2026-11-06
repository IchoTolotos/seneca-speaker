// Minimal Arduino shim for the vendored AirPlayKnob sender (ESP-IDF build): logging + millis().
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static inline uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline void delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

class GlsSerialShim {
public:
    int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[256];
        va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        size_t l = strlen(buf); while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
        if (l) ESP_LOGI("airplay", "%s", buf);
        return n;
    }
    void println(const char *s = "") { if (s && s[0]) ESP_LOGI("airplay", "%s", s); }
    void print(const char *s) { if (s && s[0]) ESP_LOGI("airplay", "%s", s); }
};
extern GlsSerialShim Serial;
