#include "chime.h"

#include <math.h>
#include <stdint.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "xvf3800_i2s.h"

static const char *TAG = "chime";

#define RATE 16000
#define NOTE_MS 110
#define GAP_MS 25
#define LEVEL 0.22f // of full scale; the volume setting applies on top

static QueueHandle_t s_queue;

// Soft bell-like note: sine + a little 2nd harmonic, 4 ms attack, exponential decay.
static size_t render_note(int16_t *out, float hz, int ms)
{
    size_t n = (size_t)RATE * ms / 1000;
    for (size_t i = 0; i < n; i++) {
        float t = (float)i / RATE;
        float env = (i < RATE * 4 / 1000 ? (float)i / (RATE * 4 / 1000) : 1.0f) * expf(-t * 18.0f);
        float s = sinf(2.0f * (float)M_PI * hz * t) + 0.25f * sinf(4.0f * (float)M_PI * hz * t);
        out[i] = (int16_t)(s * env * LEVEL * 32767.0f / 1.25f);
    }
    return n;
}

static void chime_task(void *arg)
{
    const size_t max = (size_t)RATE * 2; // test tone; the chimes are much shorter
    int16_t *pcm = heap_caps_calloc(max, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    gls_chime_t which;
    for (;;) {
        if (xQueueReceive(s_queue, &which, portMAX_DELAY) != pdTRUE || !pcm) {
            continue;
        }
        if (which == GLS_CHIME_TEST) {
            // 440 Hz at -12 dBFS with 20 ms fades: a clean tone makes clicks/static easy to hear.
            for (size_t i = 0; i < max; i++) {
                float f = i < RATE / 50 ? (float)i / (RATE / 50) : i > max - RATE / 50 ? (float)(max - i) / (RATE / 50) : 1.0f;
                pcm[i] = (int16_t)(sinf(2.0f * (float)M_PI * 440.0f * i / RATE) * f * 0.25f * 32767.0f);
            }
            esp_err_t err = xvf_i2s_play_overlay(pcm, max);
            ESP_LOGI(TAG, "test sound %s", err == ESP_OK ? "played" : esp_err_to_name(err));
            continue;
        }
        if (which == GLS_CHIME_WAKE_TEST) {
            size_t n = render_note(pcm, 1318.5f, 150); // E6: clearly distinct from the call chimes
            xvf_i2s_play_overlay(pcm, n);
            continue;
        }
        // E5 = 659 Hz, A5 = 880 Hz: up when connected, down when disconnected.
        float a = which == GLS_CHIME_CONNECTED ? 659.25f : 880.0f;
        float b = which == GLS_CHIME_CONNECTED ? 880.0f : 659.25f;
        size_t n = render_note(pcm, a, NOTE_MS);
        for (size_t i = 0; i < (size_t)RATE * GAP_MS / 1000; i++) {
            pcm[n++] = 0;
        }
        n += render_note(pcm + n, b, NOTE_MS + 60); // second note rings a bit longer
        esp_err_t err = xvf_i2s_play_overlay(pcm, n);
        ESP_LOGI(TAG, "%s chime %s", which == GLS_CHIME_CONNECTED ? "connected" : "disconnected",
                 err == ESP_OK ? "played" : esp_err_to_name(err));
    }
}

void gls_chime_play(gls_chime_t which)
{
    if (!s_queue) {
        s_queue = xQueueCreate(4, sizeof(gls_chime_t));
        if (!s_queue || xTaskCreatePinnedToCoreWithCaps(chime_task, "chime", 3072, NULL, 3, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "chime task not started");
            return;
        }
    }
    xQueueSend(s_queue, &which, 0);
}
