#include "audio_diag.h"

#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define WINDOW 1600 // 100 ms at 16 kHz
#define REC_SAMPLES (GLS_DIAG_REC_SECONDS * GLS_DIAG_REC_RATE)
#define EPISODE_START 35 // wake confidence that opens a "near miss" episode
#define EPISODE_END 20

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// mic window accumulators (microphone task only) + published values (under s_lock)
static double s_mic_sum2;
static int s_mic_peak, s_mic_n;
static double s_out_sum2;
static int s_out_n;
static volatile uint32_t s_out_last_ms;
static gls_diag_levels_t s_levels = {.mic_rms_db = -120, .mic_peak_db = -120, .mic_noise_db = -120, .out_rms_db = -120, .out_peak_db = -120};
static float s_noise_db = -120;

static gls_diag_wake_event_t s_events[GLS_DIAG_EVENTS];
static int s_event_head, s_event_count;
static bool s_in_episode, s_episode_detected;
static int s_episode_peak;
static uint32_t s_episode_t;

static int16_t *s_rec;
static size_t s_rec_pos;
static int s_rec_state;
static bool s_rec_out; // recording source: false = microphone, true = speaker output
static int s_out_peak;

static float to_db(double sum2, int n)
{
    if (n <= 0 || sum2 <= 0) return -120;
    double rms = sqrt(sum2 / n) / 32768.0;
    float db = (float)(20.0 * log10(rms));
    return db < -120 ? -120 : db;
}

void gls_diag_init(void)
{
    if (!s_rec) s_rec = heap_caps_malloc(REC_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void gls_diag_mic(const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int v = pcm[i];
        int a = v < 0 ? -v : v;
        s_mic_sum2 += (double)v * v;
        if (a > s_mic_peak) s_mic_peak = a;
        if (a >= 32700) s_levels.mic_clips++; // single writer; read as a whole under the lock
    }
    s_mic_n += (int)n;
    if (s_rec && s_rec_state == 1 && !s_rec_out) {
        size_t take = n < REC_SAMPLES - s_rec_pos ? n : REC_SAMPLES - s_rec_pos;
        memcpy(s_rec + s_rec_pos, pcm, take * sizeof(int16_t));
        s_rec_pos += take;
        if (s_rec_pos >= REC_SAMPLES) s_rec_state = 2;
    }
    if (s_mic_n >= WINDOW) {
        float rms = to_db(s_mic_sum2, s_mic_n);
        float peak = s_mic_peak ? (float)(20.0 * log10(s_mic_peak / 32768.0)) : -120;
        // Noise floor: follows quiet windows down immediately, rises slowly (~3 dB/s).
        if (rms < s_noise_db) s_noise_db = rms;
        else s_noise_db += 0.3f;
        portENTER_CRITICAL(&s_lock);
        s_levels.mic_rms_db = rms;
        s_levels.mic_peak_db = peak;
        s_levels.mic_noise_db = s_noise_db;
        portEXIT_CRITICAL(&s_lock);
        s_mic_sum2 = 0;
        s_mic_peak = 0;
        s_mic_n = 0;
    }
}

void gls_diag_out(const int16_t *pcm, size_t n, uint32_t limited)
{
    for (size_t i = 0; i < n; i++) {
        s_out_sum2 += (double)pcm[i] * pcm[i];
        int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > s_out_peak) s_out_peak = a;
    }
    s_out_n += (int)n;
    s_levels.out_limited += limited; // single writer (under the I2S TX lock)
    if (s_rec && s_rec_state == 1 && s_rec_out) {
        size_t take = n < REC_SAMPLES - s_rec_pos ? n : REC_SAMPLES - s_rec_pos;
        memcpy(s_rec + s_rec_pos, pcm, take * sizeof(int16_t));
        s_rec_pos += take;
        if (s_rec_pos >= REC_SAMPLES) s_rec_state = 2;
    }
    if (s_out_n >= WINDOW) {
        float db = to_db(s_out_sum2, s_out_n);
        float peak = s_out_peak ? (float)(20.0 * log10(s_out_peak / 32768.0)) : -120;
        portENTER_CRITICAL(&s_lock);
        s_levels.out_rms_db = db;
        s_levels.out_peak_db = peak;
        portEXIT_CRITICAL(&s_lock);
        s_out_peak = 0;
        s_out_last_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_out_sum2 = 0;
        s_out_n = 0;
    }
}

void gls_diag_wake_score(int score, bool detected)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_in_episode && (score >= EPISODE_START || detected)) {
        s_in_episode = true;
        s_episode_peak = 0;
        s_episode_detected = false;
    }
    if (!s_in_episode) return;
    if (score > s_episode_peak) {
        s_episode_peak = score;
        s_episode_t = now;
    }
    if (detected) s_episode_detected = true;
    if (score < EPISODE_END || detected) {
        // A detection resets the model history, so close the episode right away.
        portENTER_CRITICAL(&s_lock);
        s_events[s_event_head] = (gls_diag_wake_event_t){s_episode_t, (uint8_t)s_episode_peak, s_episode_detected};
        s_event_head = (s_event_head + 1) % GLS_DIAG_EVENTS;
        if (s_event_count < GLS_DIAG_EVENTS) s_event_count++;
        portEXIT_CRITICAL(&s_lock);
        s_in_episode = false;
    }
}

void gls_diag_levels(gls_diag_levels_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_levels;
    portEXIT_CRITICAL(&s_lock);
    // Stale playback level: nothing written for a while means silence.
    if ((uint32_t)(esp_timer_get_time() / 1000) - s_out_last_ms > 300) out->out_rms_db = out->out_peak_db = -120;
}

int gls_diag_wake_events(gls_diag_wake_event_t *out, int max)
{
    portENTER_CRITICAL(&s_lock);
    int n = s_event_count < max ? s_event_count : max;
    for (int i = 0; i < n; i++) {
        int idx = (s_event_head - 1 - i + GLS_DIAG_EVENTS) % GLS_DIAG_EVENTS;
        out[i] = s_events[idx];
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void gls_diag_dropout(void)
{
    portENTER_CRITICAL(&s_lock);
    s_levels.out_dropouts++;
    portEXIT_CRITICAL(&s_lock);
}

void gls_diag_record_start(bool speaker_output)
{
    if (!s_rec) return;
    s_rec_out = speaker_output;
    s_rec_pos = 0;
    s_rec_state = 1;
}

int gls_diag_record_state(int *progress_pct)
{
    if (progress_pct) *progress_pct = (int)(s_rec_pos * 100 / REC_SAMPLES);
    return s_rec_state;
}

const int16_t *gls_diag_record_samples(size_t *count)
{
    if (s_rec_state != 2) return NULL;
    *count = REC_SAMPLES;
    return s_rec;
}
