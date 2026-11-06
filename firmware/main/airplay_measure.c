// Acoustic AirPlay measurement: the speaker's own microphones listen to the AirPlay receiver.
// For each requested latency: connect, send a short beep -> measured push-to-sound delay; then a pulsed
// tone, FLUSH (+ stop feeding) once it is audible -> measured time until the receiver is silent.
// The best working latency and its measured delay are stored and used for calls (AEC reference).
#include "airplay_measure.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "airplay_tx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xvf3800_i2s.h"

static const char *TAG = "ap_measure";

#define MAX_BLOCKS 1200  // 10 ms mic blocks = 12 s
static float *s_env;     // RMS per 10 ms block
static int64_t s_env_t0;
static atomic_int s_env_n;
static atomic_bool s_running;
static char s_result[480] = "not run yet";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_result(const char *txt)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_result, txt, sizeof(s_result));
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "%s", txt);
}

static void mic_tap(const int16_t *pcm, size_t n)
{
    int i = atomic_load(&s_env_n);
    if (i >= MAX_BLOCKS) return;
    double s = 0;
    for (size_t k = 0; k < n; k++) s += (double)pcm[k] * pcm[k];
    s_env[i] = (float)sqrt(s / n);
    atomic_store(&s_env_n, i + 1);
}

static int64_t block_time_us(int i) { return s_env_t0 + (int64_t)i * 10000; }
static int block_at(int64_t t_us) { return (int)((t_us - s_env_t0) / 10000); }

// Mean RMS of blocks [a, b)
static float mean(int a, int b)
{
    double s = 0; int n = 0;
    for (int i = a; i < b && i < atomic_load(&s_env_n); i++) { s += s_env[i]; n++; }
    return n ? (float)(s / n) : 0;
}

// First block >= from where the envelope exceeds thr for `hold` consecutive blocks (waits up to timeout).
static int wait_onset(int from, float thr, int hold, int timeout_ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int run = 0;
    for (int i = from;; ) {
        while (i >= atomic_load(&s_env_n)) {
            if (esp_timer_get_time() > until) return -1;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        run = s_env[i] > thr ? run + 1 : 0;
        if (run >= hold) return i - hold + 1;
        i++;
    }
}

// First block >= from after which the envelope stays below thr for `hold` blocks.
static int wait_silence(int from, float thr, int hold, int timeout_ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int run = 0;
    for (int i = from;; ) {
        while (i >= atomic_load(&s_env_n)) {
            if (esp_timer_get_time() > until) return -1;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        run = s_env[i] < thr ? run + 1 : 0;
        if (run >= hold) return i - hold + 1;
        i++;
    }
}

static int16_t *make_tone(int ms, float hz, float level, int pulse_on_ms, int pulse_off_ms, size_t *n_out)
{
    size_t n = (size_t)ms * 16;
    int16_t *p = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    int period = (pulse_on_ms + pulse_off_ms) * 16, on = pulse_on_ms * 16;
    for (size_t i = 0; i < n; i++) {
        bool active = !pulse_off_ms || (int)(i % period) < on;
        int k = pulse_off_ms ? (int)(i % period) : (int)i;
        float env = active ? (k < 32 ? k / 32.0f : 1.0f) : 0.0f;  // 2 ms fade-in per pulse
        p[i] = (int16_t)(sinf(2.0f * (float)M_PI * hz * i / 16000.0f) * env * level * 32767.0f);
    }
    *n_out = n;
    return p;
}

typedef struct { int req_ms; int connected; int delay_ms; int play_ms; int flush_ms; float snr_db; } trial_t;

static void run_trial(int req_ms, trial_t *t)
{
    memset(t, 0, sizeof(*t));
    t->req_ms = req_ms;
    t->delay_ms = t->play_ms = t->flush_ms = -1;
    gls_ap_set_latency_ms(req_ms);
    if (gls_ap_stream_start() != ESP_OK) return;
    for (int i = 0; i < 80 && !gls_ap_stream_ready(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!gls_ap_stream_ready()) { gls_ap_stream_stop(); vTaskDelay(pdMS_TO_TICKS(1500)); return; }
    t->connected = 1;
    vTaskDelay(pdMS_TO_TICKS(req_ms + 700));  // let the receiver settle (it plays our silence)

    // 1) latency: noise baseline, then a 150 ms beep
    atomic_store(&s_env_n, 0);
    s_env_t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(500));
    float noise = mean(5, 50);
    float thr = noise * 4.0f + 30.0f;  // +12 dB over the room, plus an absolute floor
    size_t nb;
    int16_t *beep = make_tone(150, 1000.0f, 0.5f, 0, 0, &nb);
    if (!beep) goto done;
    int64_t t_push = esp_timer_get_time();
    gls_ap_onset_arm();
    xvf_i2s_overlay_start(beep, nb);
    int on = wait_onset(block_at(t_push), thr, 3, req_ms + 2500);
    free(beep);
    if (on < 0) goto done;
    t->delay_ms = (int)((block_time_us(on) - t_push) / 1000);
    int64_t t_sent = gls_ap_onset_us();  /* RTP time of the beep's first loud frame */
    if (t_sent > 0) t->play_ms = (int)((block_time_us(on) - t_sent) / 1000);
    float sig = mean(on, on + 10);
    t->snr_db = 20.0f * log10f((sig + 1) / (noise + 1));

    // 2) flush: pulsed tone; once audible for 400 ms, FLUSH + stop feeding, measure until silent
    vTaskDelay(pdMS_TO_TICKS(600));
    size_t nt;
    int16_t *tone = make_tone(2000, 800.0f, 0.5f, 100, 50, &nt);
    if (!tone) goto done;
    int64_t t_tone = esp_timer_get_time();
    xvf_i2s_overlay_start(tone, nt);
    int on2 = wait_onset(block_at(t_tone), thr, 3, req_ms + 2500);
    if (on2 >= 0) {
        vTaskDelay(pdMS_TO_TICKS(400));
        int64_t t_flush = esp_timer_get_time();
        xvf_i2s_overlay_stop();
        gls_ap_stream_flush();
        // silent = below threshold for 250 ms (longer than the 50 ms pulse gaps)
        int off = wait_silence(block_at(t_flush), thr, 25, req_ms + 3000);
        if (off >= 0) t->flush_ms = (int)((block_time_us(off) - t_flush) / 1000);
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    free(tone);
done:
    gls_ap_stream_stop();
    vTaskDelay(pdMS_TO_TICKS(1500));  // let the sender tear down before the next trial
}

static bool s_quick;

static void measure_task(void *arg)
{
    int lat[4] = {2000, 1000, 500, 250}, nlat = 4;
    if (s_quick && gls_ap_latency_ms() > 0) { lat[0] = gls_ap_latency_ms(); nlat = 1; }
    trial_t tr[4];
    int prev_lat = gls_ap_latency_ms();
    set_result("measuring... (beeps and tones on the AirPlay speaker, ~1 min)");
    xvf_i2s_set_ref_mute(true);
    xvf_i2s_set_mic_tap(mic_tap);
    char buf[480];
    int len = 0, best = -1;
    for (int k = 0; k < nlat; k++) {
        snprintf(buf, sizeof(buf), "measuring %d/%d: requested latency %d ms...", k + 1, nlat, lat[k]);
        set_result(buf);
        run_trial(lat[k], &tr[k]);
        ESP_LOGI(TAG, "requested %d ms: connected %d, delay %d ms (receiver %d ms), flush->silent %d ms, snr %.1f dB",
                 tr[k].req_ms, tr[k].connected, tr[k].delay_ms, tr[k].play_ms, tr[k].flush_ms, tr[k].snr_db);
        if (tr[k].connected && tr[k].delay_ms > 0 && tr[k].play_ms > 0 && tr[k].flush_ms >= 0) best = k;  // later = lower latency
    }
    xvf_i2s_set_mic_tap(NULL);
    xvf_i2s_set_ref_mute(false);
    len = snprintf(buf, sizeof(buf), "Results:");
    for (int k = 0; k < nlat; k++) {
        if (!tr[k].connected) len += snprintf(buf + len, sizeof(buf) - len, " | ask %d ms: no connection", tr[k].req_ms);
        else if (tr[k].delay_ms < 0) len += snprintf(buf + len, sizeof(buf) - len, " | ask %d ms: not heard", tr[k].req_ms);
        else len += snprintf(buf + len, sizeof(buf) - len, " | ask %d ms: sound after %d ms, silent %d ms after stop (%.0f dB)",
                             tr[k].req_ms, tr[k].delay_ms, tr[k].flush_ms, tr[k].snr_db);
    }
    if (best >= 0) {
        gls_ap_set_latency_ms(tr[best].req_ms);
        gls_ap_set_measured_delay_ms(tr[best].delay_ms);
        gls_ap_set_play_delay_ms(tr[best].play_ms);
        snprintf(buf + len, sizeof(buf) - len, " => using %d ms (measured delay %d ms, receiver %d ms) for calls",
                 tr[best].req_ms, tr[best].delay_ms, tr[best].play_ms);
    } else {
        gls_ap_set_latency_ms(prev_lat);
        snprintf(buf + len, sizeof(buf) - len, " => nothing reliable measured; settings unchanged");
    }
    set_result(buf);
    atomic_store(&s_running, false);
    vTaskDelete(NULL);
}

esp_err_t gls_airplay_measure_start(void) { return gls_airplay_measure_start_ex(false); }

esp_err_t gls_airplay_measure_start_ex(bool quick)
{
    s_quick = quick;
    if (atomic_load(&s_running)) return ESP_ERR_INVALID_STATE;
    if (!gls_ap_output(NULL)) return ESP_ERR_NOT_FOUND;
    if (!s_env) s_env = heap_caps_calloc(MAX_BLOCKS, sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_env) return ESP_ERR_NO_MEM;
    atomic_store(&s_running, true);
    // internal stack: it writes the results to NVS
    if (xTaskCreatePinnedToCore(measure_task, "ap_measure", 5120, NULL, 5, NULL, 1) != pdPASS) {
        atomic_store(&s_running, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool gls_airplay_measure_running(void) { return atomic_load(&s_running); }

void gls_airplay_measure_result(char *out, size_t len)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(out, s_result, len);
    taskEXIT_CRITICAL(&s_lock);
}
