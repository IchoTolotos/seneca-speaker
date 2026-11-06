#include "xvf3800_i2s.h"

#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wake_word.h"
#include "audio_diag.h"
#include "controller.h"
#include "sdkconfig.h"
#include "board_xvf3800.h"
#include "airplay_tx.h"
#include "xvf_ctrl.h"

static const char *TAG = "xvf_i2s";

#define CHUNK_FRAMES 160 /* 10 ms at 16 kHz */

static i2s_chan_handle_t s_tx, s_rx;
static atomic_bool s_mute = true;
static atomic_int s_gain_q15 = 32767; /* volume as Q15 */
static atomic_int s_voice_q15 = 65535; /* voice boost (downlink only) as Q15, default +6 dB */
static atomic_bool s_wake_test;        /* wake-word test: mic reaches the detector even when muted */

/* AirPlay output: the speaker's sound goes to an AirPlay receiver (~2 s later). The local I2S line,
 * which is also the XVF3800's AEC reference, then carries the SAME audio delayed by that latency
 * (amp off), so echo cancellation sees the reference when the HomePod actually plays it. */
static atomic_int s_dl_delay;          /* samples; 0 = AirPlay routing off */
static atomic_int s_ap_offset_ms;      /* user trim on top of the measured AirPlay delay */
static atomic_bool s_ref_mute;        /* measurement: keep the AEC reference silent */
static void (*volatile s_mic_tap)(const int16_t *pcm, size_t n);
static int32_t *s_rx_buf, *s_tx_buf;  /* CHUNK_FRAMES * 2 slots, internal RAM (DMA copy src/dst) */

/* Overlay (UI sounds): mixed into whatever is being played, so it never interleaves with the
 * downlink stream. When nothing else is playing, xvf_i2s_play_overlay pushes it out itself. */
#define OVERLAY_MAX (16000 * 2) /* 2 s (test sound) */
static int16_t *s_overlay;
static size_t s_overlay_len, s_overlay_pos;
static portMUX_TYPE s_overlay_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_tx_lock;
static atomic_uint s_last_write_ms;
/* Output timing: on_sent fires per finished DMA buffer -> frame index s_sent_frames starts playing at s_sent_us.
 * s_wr_frames = index of the next frame we write. Gives the exact I2S output time of every written sample. */
static volatile uint32_t s_sent_frames;
static volatile int64_t s_sent_us;
static uint32_t s_wr_frames;          /* under s_tx_lock */
static atomic_int s_ref_lead_ms = 40; /* AEC reference leads the HomePod's sound by this much (inside the 192 ms AEC filter) */

static bool IRAM_ATTR on_tx_sent(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx)
{
    s_sent_us = esp_timer_get_time();
    s_sent_frames += e->size / 8; /* 32-bit stereo */
    return false;
}

static StreamBufferHandle_t s_capture;
static StaticStreamBuffer_t s_capture_state;
static SemaphoreHandle_t s_capture_lock;
static atomic_bool s_capture_enabled;
static esp_err_t read_hardware(int16_t *out, size_t samples, uint32_t timeout_ms);

static void microphone_task(void *arg)
{
    int16_t pcm[CHUNK_FRAMES];
    for (;;) {
        if (read_hardware(pcm, CHUNK_FRAMES, 200) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        bool mute = atomic_load(&s_mute);
        // Muted: only the wake-word test lets the detector (and diagnostics) hear; the call never does.
        if (mute && !atomic_load(&s_wake_test)) memset(pcm, 0, sizeof(pcm));
        if (s_mic_tap) s_mic_tap(pcm, CHUNK_FRAMES);   /* raw mic (before mute), measurements only */
        gls_diag_mic(pcm, CHUNK_FRAMES);
        gls_wake_feed(pcm, CHUNK_FRAMES);
        if (mute) memset(pcm, 0, sizeof(pcm));
        gls_controller_microphone(pcm, CHUNK_FRAMES);
        xSemaphoreTake(s_capture_lock, portMAX_DELAY);
        if (atomic_load(&s_capture_enabled)) {
            // A bounded voice FIFO isolates network/encoder stalls from the detector.
            if (xStreamBufferSpacesAvailable(s_capture) >= sizeof(pcm))
                xStreamBufferSend(s_capture, pcm, sizeof(pcm), 0);
        }
        xSemaphoreGive(s_capture_lock);
    }
}

void xvf_i2s_set_capture_enabled(bool enabled)
{
    if (!s_capture_lock) return;
    xSemaphoreTake(s_capture_lock, portMAX_DELAY);
    atomic_store(&s_capture_enabled, false);
    if (enabled) {
        uint8_t discard[320];
        while (xStreamBufferReceive(s_capture, discard, sizeof(discard), 0)) {}
        atomic_store(&s_capture_enabled, true);
    }
    xSemaphoreGive(s_capture_lock);
}

esp_err_t xvf_i2s_read_mono16(int16_t *out, size_t samples, uint32_t timeout_ms)
{
    if (!s_capture || !atomic_load(&s_capture_enabled)) return ESP_ERR_INVALID_STATE;
    uint8_t *p = (uint8_t *)out;
    size_t remaining = samples * sizeof(int16_t);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (remaining && atomic_load(&s_capture_enabled)) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return ESP_ERR_TIMEOUT;
        size_t n = xStreamBufferReceive(s_capture, p, remaining, deadline - now);
        if (!n) return ESP_ERR_TIMEOUT;
        p += n; remaining -= n;
    }
    return remaining ? ESP_ERR_INVALID_STATE : ESP_OK;
}

/* XVF post-processing while AirPlay is routed (raw 32-bit: int32 or float) */
typedef union { int32_t i; float f; } pp_val_t;
static struct { uint8_t cmd; pp_val_t airplay, dflt, saved; } s_pp[3] = {
    {31, {.i = 15}, {.i = 0}},      /* PP_DTSENSITIVE */
    {24, {.f = 0.5f}, {.f = 1.0f}}, /* PP_GAMMA_E */
    {25, {.f = 0.7f}, {.f = 1.0f}}, /* PP_GAMMA_ETAIL */
};

static void airplay_route_task(void *arg)
{
    int64_t last_lvl = 0;
    bool routed = false;
    for (;;) {
        bool ready = gls_ap_stream_ready();
        if (ready && !routed) {
            if (gls_ap_play_delay_ms() <= 0) {
                /* not measured with this firmware yet: rough guess from the old push->sound value */
                int guess = (gls_ap_measured_delay_ms() > 0 ? gls_ap_measured_delay_ms() : gls_ap_stream_delay_ms()) - 150;
                gls_ap_use_play_delay_ms(guess > 50 ? guess : 50); /* PSRAM stack: no NVS writes here */
                ESP_LOGW(TAG, "AirPlay delay not measured yet: guessing %d ms (run 'Measure' in the setup page)", gls_ap_play_delay_ms());
            }
            int ms = gls_ap_play_delay_ms();
            atomic_store(&s_dl_delay, 1);
            gls_xvf_amp_enable(false);
            /* The reference is sample-exact now, so the linear AEC does the work: favour the near-end talker
             * in double-talk and over-subtract less (defaults 0 / 1.0 / 1.0 muted normal speech under GPT). */
            for (int k = 0; k < 3; k++) if (gls_xvf_read(17, s_pp[k].cmd, (uint8_t *)&s_pp[k].saved, 4) != ESP_OK) s_pp[k].saved = s_pp[k].dflt;
            for (int k = 0; k < 3; k++) gls_xvf_write(17, s_pp[k].cmd, (const uint8_t *)&s_pp[k].airplay, 4);
            routed = true;
            ESP_LOGI(TAG, "output -> AirPlay, receiver delay %d ms, AEC reference lead %d ms, trim %d ms, double-talk 15, echo gamma 0.5/0.7",
                     ms, atomic_load(&s_ref_lead_ms), atomic_load(&s_ap_offset_ms));
        } else if (!ready && routed) {
            atomic_store(&s_dl_delay, 0);
            gls_xvf_amp_enable(true);
            for (int k = 0; k < 3; k++) gls_xvf_write(17, s_pp[k].cmd, (const uint8_t *)&s_pp[k].saved, 4);
            routed = false;
            ESP_LOGI(TAG, "output -> built-in speaker");
        }
        if (routed && esp_timer_get_time() - last_lvl > 1000000) {
            last_lvl = esp_timer_get_time();
            gls_diag_levels_t l;
            gls_diag_levels(&l);
            int32_t conv = -1, pchg = -1;
            float agc = -1;
            gls_xvf_read(33, 3, (uint8_t *)&conv, 4);
            gls_xvf_read(33, 0, (uint8_t *)&pchg, 4);
            gls_xvf_read(17, 13, (uint8_t *)&agc, 4);
            ESP_LOGI(TAG, "airplay: mic %.0f dBFS (peak %.0f, noise %.0f), out %.0f dBFS, aec conv %d path %d agc %.1f, queue %d ms",
                     l.mic_rms_db, l.mic_peak_db, l.mic_noise_db, l.out_rms_db, (int)conv, (int)pchg, agc, gls_ap_stream_queue_samples() / 16);
        }
        if (routed) {
            /* no stream writing: keep the clock running (silence into AirPlay, the reference keeps following the receiver) */
            unsigned now = (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (now - atomic_load(&s_last_write_ms) >= 25) {
                xvf_i2s_write_mono16(NULL, CHUNK_FRAMES, 200);
                continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void xvf_i2s_airplay_flush(void)
{
    if (atomic_load(&s_dl_delay) > 0) {
        gls_ap_stream_flush();
    }
}

void xvf_i2s_set_airplay_offset_ms(int ms) { atomic_store(&s_ap_offset_ms, ms); }
int xvf_i2s_airplay_offset_ms(void) { return atomic_load(&s_ap_offset_ms); }
bool xvf_i2s_airplay_routed(void) { return atomic_load(&s_dl_delay) > 0; }

esp_err_t xvf_i2s_init(void)
{
    if (s_tx) {
        return ESP_OK;
    }
#if !GLS_I2S_ESP_IS_MASTER
#error "ESP32-as-slave (XVF3800 i2s_master_48k firmware) is not implemented yet"
#endif
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(GLS_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;      /* 15 ms per DMA buffer */
    chan_cfg.auto_clear_after_cb = true; /* TX underrun -> silence, not a repeated buffer */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(GLS_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GLS_I2S_BCLK_GPIO,
            .ws = GLS_I2S_WS_GPIO,
            .dout = GLS_I2S_DOUT_GPIO,
            .din = GLS_I2S_DIN_GPIO,
            .invert_flags = {0},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "init rx");

    s_rx_buf = heap_caps_malloc(CHUNK_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_tx_buf = heap_caps_malloc(CHUNK_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_tx_lock = xSemaphoreCreateMutex(); /* serializes stream writes and overlay pushes */
    gls_diag_init();
    s_overlay = heap_caps_malloc(OVERLAY_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_buf || !s_tx_buf) {
        return ESP_ERR_NO_MEM;
    }
    i2s_event_callbacks_t cbs = {.on_sent = on_tx_sent};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_tx, &cbs, NULL), TAG, "tx callback");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable rx");
    xvf_i2s_set_volume(CONFIG_GLS_DEFAULT_VOLUME);
    xvf_eq_t eq = XVF_EQ_DEFAULT;
    xvf_i2s_set_eq(&eq);
    ESP_LOGI(TAG, "I2S%d master %d Hz 32-bit stereo Philips: BCLK %d WS %d DOUT %d DIN %d (mic slot %d)",
             GLS_I2S_PORT, GLS_I2S_SAMPLE_RATE, GLS_I2S_BCLK_GPIO, GLS_I2S_WS_GPIO, GLS_I2S_DOUT_GPIO,
             GLS_I2S_DIN_GPIO, CONFIG_GLS_MIC_SLOT);
    uint8_t *storage = heap_caps_malloc(16385, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_capture_lock = xSemaphoreCreateMutex();
    if (!storage || !s_capture_lock) return ESP_ERR_NO_MEM;
    s_capture = xStreamBufferCreateStatic(16385, 1, storage, &s_capture_state);
    if (!s_capture || xTaskCreatePinnedToCore(microphone_task, "microphone", 3072, NULL, 16, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCoreWithCaps(airplay_route_task, "ap_route", 3072, NULL, 15, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static esp_err_t read_hardware(int16_t *out, size_t samples, uint32_t timeout_ms)
{
    if (!s_rx) {
        return ESP_ERR_INVALID_STATE;
    }
    while (samples > 0) {
        size_t frames = samples < CHUNK_FRAMES ? samples : CHUNK_FRAMES;
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, s_rx_buf, frames * 2 * sizeof(int32_t), &got, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        size_t n = got / (2 * sizeof(int32_t));
        for (size_t i = 0; i < n; i++) {
            // Left-justified signed 32-bit: the top 16 bits are the 16-bit sample.
            out[i] = (int16_t)(s_rx_buf[2 * i + CONFIG_GLS_MIC_SLOT] >> 16); /* mute applied by the caller */
        }
        out += n;
        samples -= n;
    }
    return ESP_OK;
}

/* ---- speaker EQ: 4 biquads (RBJ cookbook), transposed direct form II, float (S3 has an FPU) ---- */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;
static biquad_t s_eq[4];
static bool s_eq_on;

static inline float biquad_run(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static void biquad_set(biquad_t *f, double b0, double b1, double b2, double a0, double a1, double a2)
{
    *f = (biquad_t){(float)(b0 / a0), (float)(b1 / a0), (float)(b2 / a0), (float)(a1 / a0), (float)(a2 / a0), 0, 0};
}

static void biquad_pass(biquad_t *f) { *f = (biquad_t){1, 0, 0, 0, 0, 0, 0}; }

static void biquad_hpf(biquad_t *f, double hz, double q)
{
    double w = 2 * M_PI * hz / 16000.0, c = cos(w), al = sin(w) / (2 * q);
    biquad_set(f, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

static void biquad_peak(biquad_t *f, double hz, double q, double db)
{
    if (db == 0) { biquad_pass(f); return; }
    double A = pow(10, db / 40), w = 2 * M_PI * hz / 16000.0, c = cos(w), al = sin(w) / (2 * q);
    biquad_set(f, 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

static void biquad_highshelf(biquad_t *f, double hz, double db)
{
    if (db == 0) { biquad_pass(f); return; }
    double A = pow(10, db / 40), w = 2 * M_PI * hz / 16000.0, c = cos(w), s = sin(w);
    double al = s / 2 * sqrt(2), sa = 2 * sqrt(A) * al; /* shelf slope S = 1 */
    biquad_set(f, A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
               (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}

bool xvf_eq_valid(const xvf_eq_t *e)
{
    return e && (e->hpf_hz == 0 || (e->hpf_hz >= 40 && e->hpf_hz <= 300)) && e->warmth_db >= -6 &&
           e->warmth_db <= 9 && e->presence_db >= -6 && e->presence_db <= 6 && e->treble_db >= -12 && e->treble_db <= 6;
}

void xvf_i2s_set_eq(const xvf_eq_t *e)
{
    if (!xvf_eq_valid(e)) return;
    biquad_t f[4];
    if (e->hpf_hz) biquad_hpf(&f[0], e->hpf_hz, 0.707); else biquad_pass(&f[0]);
    biquad_peak(&f[1], 400, 0.9, e->warmth_db);
    biquad_peak(&f[2], 2500, 1.0, e->presence_db);
    biquad_highshelf(&f[3], 5000, e->treble_db);
    if (s_tx_lock) xSemaphoreTake(s_tx_lock, portMAX_DELAY); /* never mid-block */
    memcpy(s_eq, f, sizeof(f));
    s_eq_on = e->on;
    if (s_tx_lock) xSemaphoreGive(s_tx_lock);
}

/* Above 100 % the gain exceeds 1: peaks above LIM_T are compressed smoothly towards full scale
 * instead of hard-clipping (hard clipping crackles). Below LIM_T the signal is untouched. */
#define LIM_T 24000
static inline int32_t soft_limit(int32_t v)
{
    int32_t a = v < 0 ? -v : v;
    if (a <= LIM_T) return v;
    const int32_t k = 32767 - LIM_T;
    int32_t d = a - LIM_T;
    a = LIM_T + (int32_t)((int64_t)d * k / (d + k)); /* < 32767 for any input */
    return v < 0 ? -a : a;
}

esp_err_t xvf_i2s_write_mono16(const int16_t *in, size_t samples, uint32_t timeout_ms)
{
    if (!s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    int32_t gain = atomic_load(&s_gain_q15);
    int32_t voice = atomic_load(&s_voice_q15);
    if (s_tx_lock) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    }
    if (in) { /* only real stream writes count; the overlay's own silence pushes must not */
        unsigned now = (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        atomic_store(&s_last_write_ms, now);
    }
    while (samples > 0) {
        size_t frames = samples < CHUNK_FRAMES ? samples : CHUNK_FRAMES;
        int16_t played[CHUNK_FRAMES];
        uint32_t limited = 0;
        /* when does this chunk leave the I2S line? */
        uint32_t sent;
        int64_t sent_us;
        do { sent = s_sent_frames; sent_us = s_sent_us; } while (sent != s_sent_frames);
        if ((int32_t)(s_wr_frames - sent) < 240) s_wr_frames = sent + 240; /* DMA ran dry: we land after the playing buffer */
        int64_t t_out = sent_us + (int64_t)(int32_t)(s_wr_frames - sent) * 1000000 / GLS_I2S_SAMPLE_RATE;
        int16_t ref[CHUNK_FRAMES];
        bool routed = atomic_load(&s_dl_delay) > 0;
        bool have_ref = routed && !atomic_load(&s_ref_mute) &&
                        gls_ap_ref_read(t_out + (int64_t)(atomic_load(&s_ref_lead_ms) - atomic_load(&s_ap_offset_ms)) * 1000, ref, frames);
        for (size_t i = 0; i < frames; i++) {
            int32_t x = in ? (int32_t)(((int64_t)in[i] * voice) >> 15) : 0;
            portENTER_CRITICAL(&s_overlay_lock);
            if (s_overlay_pos < s_overlay_len) {
                x += s_overlay[s_overlay_pos++];
            }
            portEXIT_CRITICAL(&s_overlay_lock);
            if (routed) {
                /* AirPlay: the receiver gets the plain mix (its own volume applies); the local line carries
                 * exactly what the receiver is playing at this sample's output time, as the AEC reference
                 * (from the sender's RTP-indexed history); amp is off. */
                int16_t xm = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
                gls_ap_stream_push(&xm, 1);
                x = have_ref ? ref[i] : 0;
            } else if (s_eq_on) {
                float y = (float)x;
                for (int b = 0; b < 4; b++) y = biquad_run(&s_eq[b], y);
                x = y > 2e9f ? 2000000000 : y < -2e9f ? -2000000000 : (int32_t)y;
            }
            int32_t v = (int32_t)(((int64_t)x * gain) >> 15); /* gains > 1: 64-bit product */
            if (v > LIM_T || v < -LIM_T) limited++;
            v = soft_limit(v);
            played[i] = (int16_t)v;
            int32_t s32 = v << 16;
            s_tx_buf[2 * i] = s32;     /* identical L and R: AEC reference + DAC */
            s_tx_buf[2 * i + 1] = s32;
        }
        gls_diag_out(played, frames, limited);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_tx_buf, frames * 2 * sizeof(int32_t), &written, timeout_ms);
        if (err != ESP_OK) {
            if (s_tx_lock) {
                xSemaphoreGive(s_tx_lock);
            }
            return err;
        }
        s_wr_frames += written / (2 * sizeof(int32_t));
        if (in) {
            in += frames;
        }
        samples -= frames;
    }
    if (s_tx_lock) {
        xSemaphoreGive(s_tx_lock);
    }
    return ESP_OK;
}

esp_err_t xvf_i2s_play_overlay(const int16_t *pcm, size_t samples)
{
    if (!s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_overlay || !s_tx_lock) {
        return ESP_ERR_NO_MEM;
    }
    if (samples > OVERLAY_MAX) {
        samples = OVERLAY_MAX;
    }
    portENTER_CRITICAL(&s_overlay_lock);
    memcpy(s_overlay, pcm, samples * sizeof(int16_t));
    s_overlay_len = samples;
    s_overlay_pos = 0;
    portEXIT_CRITICAL(&s_overlay_lock);
    // Push silence (carrying the overlay) while no stream is writing; a stream that starts
    // meanwhile simply carries the rest of the overlay.
    for (;;) {
        portENTER_CRITICAL(&s_overlay_lock);
        bool done = s_overlay_pos >= s_overlay_len;
        portEXIT_CRITICAL(&s_overlay_lock);
        if (done) {
            break;
        }
        unsigned now = (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - atomic_load(&s_last_write_ms) < 40) {
            vTaskDelay(pdMS_TO_TICKS(10)); // a stream is playing and mixes the overlay in
            continue;
        }
        esp_err_t err = xvf_i2s_write_mono16(NULL, CHUNK_FRAMES, 200);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

void xvf_i2s_set_mic_mute(bool mute)
{
    atomic_store(&s_mute, mute);
    ESP_LOGI(TAG, "mic %s", mute ? "muted" : "unmuted");
}

bool xvf_i2s_mic_muted(void)
{
    return atomic_load(&s_mute);
}

void xvf_i2s_set_voice_boost(int db)
{
    if (db < 0) db = 0;
    if (db > XVF_VOICE_BOOST_MAX_DB) db = XVF_VOICE_BOOST_MAX_DB;
    atomic_store(&s_voice_q15, (int)(32768.0 * pow(10.0, db / 20.0)));
}

void xvf_i2s_set_wake_test(bool on) { atomic_store(&s_wake_test, on); }
void xvf_i2s_set_mic_tap(void (*fn)(const int16_t *pcm, size_t n)) { s_mic_tap = fn; }
void xvf_i2s_set_ref_mute(bool on) { atomic_store(&s_ref_mute, on); }
void xvf_i2s_overlay_stop(void)
{
    portENTER_CRITICAL(&s_overlay_lock);
    s_overlay_pos = s_overlay_len;
    portEXIT_CRITICAL(&s_overlay_lock);
}
void xvf_i2s_overlay_start(const int16_t *pcm, size_t samples)
{
    /* like play_overlay but returns at once (the AirPlay route task keeps the writer running) */
    if (!s_overlay) return;
    if (samples > OVERLAY_MAX) samples = OVERLAY_MAX;
    portENTER_CRITICAL(&s_overlay_lock);
    memcpy(s_overlay, pcm, samples * sizeof(int16_t));
    s_overlay_len = samples;
    s_overlay_pos = 0;
    portEXIT_CRITICAL(&s_overlay_lock);
}
bool xvf_i2s_wake_test(void) { return atomic_load(&s_wake_test); }

void xvf_i2s_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > XVF_VOLUME_MAX) {
        percent = XVF_VOLUME_MAX;
    }
    // simple perceptual curve: gain = (p/100)^2; above 100 % up to 2.25x (+7 dB), soft-limited
    int32_t g = (int32_t)((int64_t)32767 * percent * percent / 10000);
    atomic_store(&s_gain_q15, g);
}
