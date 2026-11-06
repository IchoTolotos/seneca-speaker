#include "media.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "audio_diag.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "audio_render.h"
#include "av_render.h"
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_capture.h"
#include "esp_capture_audio_src_if.h"
#include "esp_capture_sink.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"

#include "board_xvf3800.h"
#include "xvf3800_i2s.h"

static const char *TAG = "media";
static atomic_uint s_suppress_until_ms;

static bool playback_suppressed(void)
{
    uint32_t until = atomic_load(&s_suppress_until_ms);
    return until && (int32_t)(until - (uint32_t)(esp_timer_get_time() / 1000)) > 0;
}

// ---------------------------------------------------------------------------
// Jitter buffer. Downlink packets arrive in bursts (measured: no loss, but 3-9 gaps of 60-245 ms
// every 5 s); playing each packet on arrival empties the 90 ms I2S DMA in every gap -> clicks.
// Decoded PCM goes into a PSRAM ring; a playout task starts once JB_PREFILL_MS is buffered (or the
// utterance ended) and re-buffers after an underrun.
// ---------------------------------------------------------------------------
#define JB_RATE        16000
#define JB_PREFILL_MS  200  /* starting target; adapts between MIN and MAX */
#define JB_MIN_MS      200
#define JB_MAX_MS      800
#define JB_STEP_MS     100  /* grow per underrun */
#define JB_RELAX_MS    30000 /* shrink by 50 ms after this long without an underrun */
#define JB_CAP_MS      3000
#define JB_CHUNK       160 /* 10 ms */
#define JB_BYTES(ms)   ((size_t)(ms) * JB_RATE / 1000 * sizeof(int16_t))

static StreamBufferHandle_t s_jb;
static StaticStreamBuffer_t s_jb_state;
static atomic_bool s_jb_flush;
static atomic_uint s_jb_last_in_ms;
static atomic_uint s_jb_underruns, s_jb_stretched, s_jb_dropped;
static atomic_uint s_jb_target_ms = JB_PREFILL_MS;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void playout_task(void *arg)
{
    int16_t chunk[JB_CHUNK];
    bool playing = false;
    // The server delivers slightly slower than real time on average (measured ~237-246 instead
    // of 250 packets per 5 s) plus bursts, so a fixed buffer slowly drains: adapt the target.
    uint32_t target_ms = JB_PREFILL_MS, last_change = now_ms();
    for (;;) {
        if (atomic_exchange(&s_jb_flush, false)) {
            size_t n;
            while ((n = xStreamBufferReceive(s_jb, chunk, sizeof(chunk), 0)) > 0) {} // discard
            playing = false;
        }
        size_t avail = xStreamBufferBytesAvailable(s_jb);
        bool idle_in = now_ms() - atomic_load(&s_jb_last_in_ms) > 80; // no new audio: utterance end
        if (!playing) {
            if (now_ms() - last_change > JB_RELAX_MS && target_ms > JB_MIN_MS) {
                target_ms -= 50;
                last_change = now_ms();
                atomic_store(&s_jb_target_ms, target_ms);
            }
            if (avail >= JB_BYTES(target_ms) || (avail > 0 && idle_in)) {
                playing = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }
        if (avail < sizeof(chunk) && !idle_in) {
            // Underrun mid-speech: wait for the buffer to refill instead of playing gaps.
            playing = false;
            atomic_fetch_add(&s_jb_underruns, 1);
            if (target_ms < JB_MAX_MS) target_ms += JB_STEP_MS;
            last_change = now_ms();
            atomic_store(&s_jb_target_ms, target_ms);
            gls_diag_dropout();
            continue;
        }
        size_t got = xStreamBufferReceive(s_jb, chunk, sizeof(chunk), 0);
        if (got == 0) {
            playing = false;
            continue;
        }
        if (got < sizeof(chunk)) memset((uint8_t *)chunk + got, 0, sizeof(chunk) - got);
        // Time-scale in pauses only (NetEq-lite): the server delivers ~3 % slower than real time, so
        // stretch quiet 10 ms slices when the buffer runs low and drop them when it is far too full.
        int peak = 0;
        for (int i = 0; i < JB_CHUNK; i++) { int a = chunk[i] < 0 ? -chunk[i] : chunk[i]; if (a > peak) peak = a; }
        bool quiet = peak < 330; // below about -40 dBFS: a pause between words
        size_t fill = xStreamBufferBytesAvailable(s_jb);
        if (quiet && fill > JB_BYTES(target_ms * 2)) {
            atomic_fetch_add(&s_jb_dropped, 1);
            continue; // skip this quiet slice
        }
        xvf_i2s_write_mono16(chunk, JB_CHUNK, 200); // paced by the I2S DMA
        if (quiet && fill < JB_BYTES(target_ms * 3 / 4)) {
            static const int16_t silence[JB_CHUNK];
            xvf_i2s_write_mono16(silence, JB_CHUNK, 200);
            atomic_fetch_add(&s_jb_stretched, 1);
        }
    }
}

static esp_err_t jitter_buffer_init(void)
{
    uint8_t *storage = heap_caps_malloc(JB_BYTES(JB_CAP_MS) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) return ESP_ERR_NO_MEM;
    s_jb = xStreamBufferCreateStatic(JB_BYTES(JB_CAP_MS) + 1, 1, storage, &s_jb_state);
    if (!s_jb || xTaskCreatePinnedToCore(playout_task, "playout", 3072, NULL, 19, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static void jitter_buffer_push(const int16_t *pcm, size_t samples)
{
    atomic_store(&s_jb_last_in_ms, now_ms());
    // Blocks when 3 s are queued (back-pressure to the decoder); never drops speech.
    xStreamBufferSend(s_jb, pcm, samples * sizeof(int16_t), pdMS_TO_TICKS(500));
    atomic_store(&s_jb_last_in_ms, now_ms());
}

uint32_t gls_media_underruns(void) { return atomic_load(&s_jb_underruns); }
uint32_t gls_media_buffer_ms(void) { return atomic_load(&s_jb_target_ms); }
uint32_t gls_media_stretched(void) { return atomic_load(&s_jb_stretched); }
uint32_t gls_media_dropped(void) { return atomic_load(&s_jb_dropped); }

#define SAMPLE_RATE GLS_I2S_SAMPLE_RATE

static struct {
    esp_capture_handle_t       capture;
    esp_capture_sink_handle_t  sink;
    bool                       capturing;
    audio_render_handle_t      render;
    av_render_handle_t         player;
    bool                       stream_added;
} m;

// ---------------------------------------------------------------------------
// Thread scheduling (names from esp_capture / av_render / our own tasks).
// Opus encode/decode need big stacks; media_lib puts them in PSRAM
// (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY).
// ---------------------------------------------------------------------------

static void thread_scheduler(const char *name, media_lib_thread_cfg_t *cfg)
{
    if (!strcmp(name, "aenc_0")) {          // Opus encoder
        cfg->stack_size = 40 * 1024;
        cfg->priority = 10;
        cfg->core_id = 1;
    } else if (!strcmp(name, "AUD_SRC")) {  // capture source reader
        cfg->stack_size = 8 * 1024;
        cfg->priority = 15;
        cfg->core_id = 1;
    } else if (!strcmp(name, "buffer_in")) {
        cfg->stack_size = 6 * 1024;
        cfg->priority = 10;
        cfg->core_id = 0;
    } else if (!strcmp(name, "Adec")) {     // Opus decoder
        cfg->stack_size = 40 * 1024;
        cfg->priority = 15;
        cfg->core_id = 0;
    } else if (!strcmp(name, "ARender")) {  // I2S writer
        cfg->stack_size = 6 * 1024;
        cfg->priority = 20;
        cfg->core_id = 0;
    } else if (!strcmp(name, "pc_task")) {  // esp_peer main loop (rtc_peer.c)
        cfg->stack_size = 25 * 1024;
        cfg->priority = 18;
        cfg->core_id = 1;
    } else if (!strcmp(name, "pc_send")) {  // Opus -> esp_peer_send_audio (rtc_peer.c)
        cfg->stack_size = 6 * 1024;
        cfg->priority = 15;
        cfg->core_id = 1;
    }
}

static void capture_scheduler(const char *name, esp_capture_thread_schedule_cfg_t *sc)
{
    media_lib_thread_cfg_t cfg = {
        .stack_size = sc->stack_size,
        .priority = sc->priority,
        .core_id = sc->core_id,
    };
    thread_scheduler(name, &cfg);
    sc->stack_size = cfg.stack_size;
    sc->priority = cfg.priority;
    sc->core_id = cfg.core_id;
    sc->stack_in_ext = true;
}

// ---------------------------------------------------------------------------
// Capture source: XVF3800 processed mic, PCM 16 kHz mono 16-bit. No AEC here.
// ---------------------------------------------------------------------------

typedef struct {
    esp_capture_audio_src_if_t base;
    esp_capture_audio_info_t   info;
    uint64_t                   frames;
    atomic_bool                started;
    atomic_bool                abort;
} xvf_src_t;

static const esp_capture_audio_info_t k_src_caps = {
    .format_id = ESP_CAPTURE_FMT_ID_PCM,
    .sample_rate = SAMPLE_RATE,
    .channel = 1,
    .bits_per_sample = 16,
};

static esp_capture_err_t src_open(esp_capture_audio_src_if_t *h)
{
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_get_codecs(esp_capture_audio_src_if_t *h, const esp_capture_format_id_t **codecs, uint8_t *num)
{
    static const esp_capture_format_id_t pcm[] = {ESP_CAPTURE_FMT_ID_PCM};
    *codecs = pcm;
    *num = 1;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_set_fixed_caps(esp_capture_audio_src_if_t *h, const esp_capture_audio_info_t *caps)
{
    return ESP_CAPTURE_ERR_OK; // the format is fixed by the hardware anyway
}

static esp_capture_err_t src_negotiate(esp_capture_audio_src_if_t *h, esp_capture_audio_info_t *in, esp_capture_audio_info_t *out)
{
    if (in->format_id != ESP_CAPTURE_FMT_ID_PCM) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    *out = k_src_caps; // capture inserts a resampler/channel converter if the sink differs
    ((xvf_src_t *)h)->info = k_src_caps;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_start(esp_capture_audio_src_if_t *h)
{
    xvf_src_t *s = (xvf_src_t *)h;
    xvf_i2s_set_capture_enabled(true);
    s->frames = 0;
    atomic_store(&s->abort, false);
    atomic_store(&s->started, true);
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_read(esp_capture_audio_src_if_t *h, esp_capture_stream_frame_t *frame)
{
    xvf_src_t *s = (xvf_src_t *)h;
    if (!atomic_load(&s->started) || atomic_load(&s->abort)) {
        return ESP_CAPTURE_ERR_NOT_SUPPORTED;
    }
    size_t samples = frame->size / sizeof(int16_t);
    if (xvf_i2s_read_mono16((int16_t *)frame->data, samples, 200) != ESP_OK) {
        return ESP_CAPTURE_ERR_INTERNAL;
    }
    frame->pts = (uint32_t)(s->frames * 1000 / SAMPLE_RATE);
    s->frames += samples;
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_abort(esp_capture_audio_src_if_t *h)
{
    atomic_store(&((xvf_src_t *)h)->abort, true);
    xvf_i2s_set_capture_enabled(false);
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_stop(esp_capture_audio_src_if_t *h)
{
    xvf_i2s_set_capture_enabled(false);
    atomic_store(&((xvf_src_t *)h)->started, false); // I2S keeps running
    return ESP_CAPTURE_ERR_OK;
}

static esp_capture_err_t src_close(esp_capture_audio_src_if_t *h)
{
    return ESP_CAPTURE_ERR_OK;
}

static xvf_src_t s_src = {
    .base = {
        .open = src_open,
        .get_support_codecs = src_get_codecs,
        .set_fixed_caps = src_set_fixed_caps,
        .negotiate_caps = src_negotiate,
        .start = src_start,
        .read_frame = src_read,
        .abort = src_abort,
        .stop = src_stop,
        .close = src_close,
    },
};

// ---------------------------------------------------------------------------
// Render: decoded PCM (fixed to 16 kHz mono 16-bit by av_render) -> I2S both slots.
// ---------------------------------------------------------------------------

typedef struct {
    av_render_audio_frame_info_t info;
} xvf_render_t;

static audio_render_handle_t r_init(void *cfg, int cfg_size)
{
    return calloc(1, sizeof(xvf_render_t));
}

static int r_open(audio_render_handle_t h, av_render_audio_frame_info_t *info)
{
    xvf_render_t *r = (xvf_render_t *)h;
    r->info = *info;
    if (info->sample_rate != SAMPLE_RATE || info->channel != 1 || info->bits_per_sample != 16) {
        ESP_LOGW(TAG, "render opened with %d Hz/%d ch/%d bit (expected 16k mono 16-bit)",
                 (int)info->sample_rate, info->channel, info->bits_per_sample);
    }
    return 0;
}

static int r_write(audio_render_handle_t h, av_render_audio_frame_t *f)
{
    xvf_render_t *r = (xvf_render_t *)h;
    if (playback_suppressed()) memset(f->data, 0, f->size);
    if (r->info.channel == 2) {
        // Downmix in place (L+R)/2 if av_render ever hands us stereo.
        int16_t *p = (int16_t *)f->data;
        int n = f->size / 4;
        for (int i = 0; i < n; i++) {
            p[i] = (int16_t)(((int32_t)p[2 * i] + p[2 * i + 1]) / 2);
        }
        jitter_buffer_push(p, n);
        return 0;
    }
    jitter_buffer_push((const int16_t *)f->data, f->size / 2);
    return 0;
}

static int r_latency(audio_render_handle_t h, uint32_t *latency)
{
    *latency = 0;
    return 0;
}

static int r_get_info(audio_render_handle_t h, av_render_audio_frame_info_t *info)
{
    *info = ((xvf_render_t *)h)->info;
    return 0;
}

static int r_set_speed(audio_render_handle_t h, float speed)
{
    return speed == 1.0f ? 0 : -1;
}

static int r_close(audio_render_handle_t h)
{
    return 0; // I2S keeps running (auto-clear -> silence)
}

static void r_deinit(audio_render_handle_t h)
{
    free(h);
}

// ---------------------------------------------------------------------------

esp_err_t gls_media_init(void)
{
    media_lib_add_default_adapter();
    media_lib_thread_set_schedule_cb(thread_scheduler);
    esp_capture_set_thread_scheduler(capture_scheduler);

    // Opus only: the linker drops every other codec (esp_audio_*_register_default would pull ~900 KB).
    esp_opus_enc_register();
    esp_opus_dec_register();

    esp_err_t err = xvf_i2s_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = jitter_buffer_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jitter buffer init failed");
        return err;
    }

    esp_capture_cfg_t ccfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_NONE,
        .audio_src = &s_src.base,
    };
    if (esp_capture_open(&ccfg, &m.capture) != ESP_CAPTURE_ERR_OK || !m.capture) {
        ESP_LOGE(TAG, "esp_capture_open failed");
        return ESP_FAIL;
    }

    static int dummy_cfg;
    audio_render_cfg_t rcfg = {
        .ops = {
            .init = r_init,
            .open = r_open,
            .write = r_write,
            .get_latency = r_latency,
            .get_frame_info = r_get_info,
            .set_speed = r_set_speed,
            .close = r_close,
            .deinit = r_deinit,
        },
        .cfg = &dummy_cfg,
        .cfg_size = sizeof(dummy_cfg),
    };
    m.render = audio_render_alloc_handle(&rcfg);
    if (!m.render) {
        ESP_LOGE(TAG, "audio render alloc failed");
        return ESP_ERR_NO_MEM;
    }
    av_render_cfg_t pcfg = {
        .audio_render = m.render,
        .audio_raw_fifo_size = 8 * 4096,
        .audio_render_fifo_size = 100 * 1024, // server RTP pauses up to ~3 s; FIFO only smooths jitter
        .allow_drop_data = false,
    };
    m.player = av_render_open(&pcfg);
    if (!m.player) {
        ESP_LOGE(TAG, "av_render_open failed");
        return ESP_FAIL;
    }
    av_render_audio_frame_info_t fixed = {
        .sample_rate = SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    av_render_set_fixed_frame_info(m.player, &fixed); // decoder output resampled/downmixed to this
    av_render_set_audio_threshold(m.player, 0);
    ESP_LOGI(TAG, "media ready (Opus only, 16 kHz mono, XVF3800 AEC)");
    return ESP_OK;
}

esp_err_t gls_media_capture_start(void)
{
    if (m.capturing) {
        return ESP_OK;
    }
    if (!m.sink) {
        esp_capture_sink_cfg_t scfg = {
            .audio_info = {
                .format_id = ESP_CAPTURE_FMT_ID_OPUS,
                .sample_rate = SAMPLE_RATE,
                .channel = 1,
                .bits_per_sample = 16,
            },
        };
        if (esp_capture_sink_setup(m.capture, 0, &scfg, &m.sink) != ESP_CAPTURE_ERR_OK || !m.sink) {
            ESP_LOGE(TAG, "capture sink setup failed");
            return ESP_FAIL;
        }
        esp_capture_sink_set_bitrate(m.sink, ESP_CAPTURE_STREAM_TYPE_AUDIO, CONFIG_GLS_OPUS_BITRATE);
    }
    esp_capture_sink_enable(m.sink, ESP_CAPTURE_RUN_MODE_ALWAYS);
    if (esp_capture_start(m.capture) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "capture start failed");
        return ESP_FAIL;
    }
    m.capturing = true;
    ESP_LOGI(TAG, "uplink started (Opus %d bps)", CONFIG_GLS_OPUS_BITRATE);
    return ESP_OK;
}

void gls_media_capture_stop(void)
{
    if (m.capturing) {
        esp_capture_stop(m.capture);
        m.capturing = false;
        ESP_LOGI(TAG, "uplink stopped");
    }
}

int gls_media_capture_drain(gls_media_frame_cb_t cb, void *ctx)
{
    if (!m.capturing || !m.sink) {
        return 0;
    }
    int n = 0;
    esp_capture_stream_frame_t f = {.stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO};
    while (esp_capture_sink_acquire_frame(m.sink, &f, true) == ESP_CAPTURE_ERR_OK) {
        cb(f.data, f.size, f.pts, ctx);
        esp_capture_sink_release_frame(m.sink, &f);
        n++;
    }
    return n;
}

esp_err_t gls_media_play_stream(uint32_t sample_rate, uint8_t channels)
{
    av_render_audio_info_t ai = {
        .codec = AV_RENDER_AUDIO_CODEC_OPUS,
        .sample_rate = sample_rate ? sample_rate : 48000,
        .channel = channels ? channels : 1,
        .bits_per_sample = 16,
    };
    ESP_LOGI(TAG, "downlink: Opus %d Hz %d ch -> 16 kHz mono", (int)ai.sample_rate, ai.channel);
    int ret = av_render_add_audio_stream(m.player, &ai);
    m.stream_added = ret == 0;
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

void gls_media_play_opus(const uint8_t *data, int size, uint32_t pts)
{
    if (!m.stream_added || playback_suppressed()) {
        return;
    }
    av_render_audio_data_t d = {
        .pts = pts,
        .data = (uint8_t *)data,
        .size = (uint32_t)size,
    };
    av_render_add_audio_data(m.player, &d);
}

void gls_media_play_suppress(uint32_t duration_ms)
{
    atomic_store(&s_suppress_until_ms, (uint32_t)(esp_timer_get_time() / 1000) + duration_ms);
}

void gls_media_play_flush(void)
{
    if (m.player) {
        av_render_flush(m.player);
    }
    atomic_store(&s_jb_flush, true); // drop buffered speech too
    xvf_i2s_airplay_flush();         // and what the AirPlay receiver still has queued
}

void gls_media_play_reset(void)
{
    atomic_store(&s_suppress_until_ms, 0);
    if (m.player) {
        av_render_reset(m.player);
    }
    atomic_store(&s_jb_flush, true);
    m.stream_added = false;
}
