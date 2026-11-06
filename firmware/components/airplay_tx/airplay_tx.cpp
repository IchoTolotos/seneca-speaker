// Glue between the vendored AirPlayKnob sender (vendor/raop_AnySpeaker.cpp, MIT) and our firmware:
// app-state stubs, persistent HomeKit client identity (NVS), mDNS discovery, a test-tone task.
#include "airplay_tx.h"

#include <math.h>
#include <string.h>
#include <strings.h>
#include "Arduino.h"
#include "app_state.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mdns.h"
#include "nvs.h"
#include "raop_AnySpeaker.h"
#include "sodium.h"

static const char *TAG = "airplay";

GlsSerialShim Serial;
AppState g_app;

static bool s_inited;
static char s_client_id[37];
static uint8_t s_client_pk[32], s_client_sk[64];
static char s_paired[MAX_SPEAKERS][20];
static int s_paired_count;
static volatile bool s_paired_dirty;
static volatile bool s_busy;
static volatile int s_volume = 70;
static volatile int s_latency_ms;           // 0 = receiver default (2000 ms)
static volatile int s_measured_delay_ms;
static volatile int s_play_ms;  // measured RTP time -> sound delay at the receiver (0 = unknown)
static RaopClient *volatile s_client;  // live session (for volume changes)
static volatile bool s_volume_dirty;
static gls_ap_speaker_t s_out;   // speaker that calls play on (persisted)
static bool s_out_set;
static char s_status[128] = "idle";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...)
{
    char buf[128];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_status, buf, sizeof(s_status));
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "%s", buf);
}

// ---------------------------------------------------------------- hooks used by the vendored sender
void app_notify_pin_toast(const char *title, const char *msg, uint32_t color)
{
    set_status("%s: %s", title ? title : "AirPlay", msg ? msg : "");
}

void app_get_ap2_client_identity(char id_out[37], uint8_t ltpk_out[32], uint8_t ltsk_out[64])
{
    memcpy(id_out, s_client_id, 37);
    memcpy(ltpk_out, s_client_pk, 32);
    memcpy(ltsk_out, s_client_sk, 64);
}

bool app_is_ap2_speaker_paired(const char *ip)
{
    if (!ip || !ip[0]) return false;
    for (int i = 0; i < s_paired_count; i++)
        if (strcmp(s_paired[i], ip) == 0) return true;
    return false;
}

void app_mark_ap2_speaker_paired(const char *ip)
{
    // Runs on the sender task (PSRAM stack): only update RAM; NVS is written later from an
    // internal-RAM task (gls_ap_status / gls_ap_discover), since flash writes disable PSRAM access.
    if (!ip || !ip[0] || app_is_ap2_speaker_paired(ip) || s_paired_count >= MAX_SPEAKERS) return;
    strlcpy(s_paired[s_paired_count++], ip, sizeof(s_paired[0]));
    s_paired_dirty = true;
}

static void persist_paired(void)
{
    if (!s_paired_dirty) return;
    nvs_handle_t h;
    if (nvs_open("gls_airplay", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "paired", s_paired, sizeof(s_paired[0]) * s_paired_count);
        nvs_commit(h);
        nvs_close(h);
        s_paired_dirty = false;
    }
}

// Settings (call output speaker, volume) are needed before any AirPlay action: load them on first access.
static bool s_settings_loaded;
static void save_u16(const char *key, uint16_t v);

static void load_settings(void)
{
    if (s_settings_loaded) return;
    s_settings_loaded = true;
    nvs_handle_t h;
    if (nvs_open("gls_airplay", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "volume", &v) == ESP_OK && v <= 100) s_volume = v;
    uint16_t u16;
    if (nvs_get_u16(h, "lat_ms", &u16) == ESP_OK) s_latency_ms = u16;
    if (nvs_get_u16(h, "meas_ms", &u16) == ESP_OK) s_measured_delay_ms = u16;
    if (nvs_get_u16(h, "play_ms", &u16) == ESP_OK) s_play_ms = u16;
    size_t ol = sizeof(s_out);
    s_out_set = nvs_get_blob(h, "output", &s_out, &ol) == ESP_OK && ol == sizeof(s_out) && s_out.ip[0];
    nvs_close(h);
}

// ---------------------------------------------------------------- init: identity + mDNS
esp_err_t gls_ap_init(void)
{
    if (s_inited) return ESP_OK;
    // libsodium's default RNG reads /dev/urandom (absent here) and aborts: use the ESP32 hardware RNG.
    static randombytes_implementation esp_rng = {
        .implementation_name = []() -> const char * { return "esp32"; },
        .random = []() -> uint32_t { return esp_random(); },
        .stir = NULL,
        .uniform = NULL,
        .buf = [](void *const buf, const size_t size) { esp_fill_random(buf, size); },
        .close = NULL,
    };
    randombytes_set_implementation(&esp_rng);
    if (sodium_init() < 0) return ESP_FAIL;
    memset(&g_app, 0, sizeof(g_app));
    g_app.mutex = xSemaphoreCreateMutex();
    g_app.current_speaker = -1;
    g_app.playing = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open("gls_airplay", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    size_t l1 = sizeof(s_client_id), l2 = sizeof(s_client_pk), l3 = sizeof(s_client_sk);
    if (nvs_get_str(h, "client_id", s_client_id, &l1) != ESP_OK || strlen(s_client_id) != 36 ||
        nvs_get_blob(h, "client_pk", s_client_pk, &l2) != ESP_OK || l2 != 32 ||
        nvs_get_blob(h, "client_sk", s_client_sk, &l3) != ESP_OK || l3 != 64) {
        // New persistent HomeKit controller identity: random UUID + Ed25519 key pair.
        uint8_t u[16];
        esp_fill_random(u, sizeof(u));
        u[6] = (u[6] & 0x0F) | 0x40; u[8] = (u[8] & 0x3F) | 0x80;
        snprintf(s_client_id, sizeof(s_client_id),
                 "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        uint8_t seed[32];
        esp_fill_random(seed, sizeof(seed));
        crypto_sign_seed_keypair(s_client_pk, s_client_sk, seed);
        nvs_set_str(h, "client_id", s_client_id);
        nvs_set_blob(h, "client_pk", s_client_pk, 32);
        nvs_set_blob(h, "client_sk", s_client_sk, 64);
        nvs_commit(h);
        ESP_LOGI(TAG, "created AirPlay client identity");
    }
    load_settings();
    size_t pl = sizeof(s_paired);
    if (nvs_get_blob(h, "paired", s_paired, &pl) == ESP_OK) s_paired_count = (int)(pl / sizeof(s_paired[0]));
    nvs_close(h);

    err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    mdns_hostname_set("seneca-speaker");
    s_inited = true;
    return ESP_OK;
}

static bool should_use_ap2(const char *am, const char *et)
{
    return (et && (strchr(et, '4') || strchr(et, '5'))) ||
           (am && (strcasestr(am, "AudioAccessory") || strcasestr(am, "AppleTV") || strcasestr(am, "HomePod") ||
                   strcasestr(am, "Mac")));
}

static void ingest(mdns_result_t *results, gls_ap_speaker_t *out, int *count, int max)
{
    for (mdns_result_t *r = results; r && *count < max; r = r->next) {
        char ip[20] = {0};
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next)
            if (a->addr.type == ESP_IPADDR_TYPE_V4) { snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a->addr.u_addr.ip4)); break; }
        if (!ip[0] || strcmp(ip, "0.0.0.0") == 0) continue;
        const char *inst = r->instance_name ? r->instance_name : "AirPlay speaker";
        const char *at = strchr(inst, '@');
        char name[48];
        strlcpy(name, (at && at[1]) ? at + 1 : inst, sizeof(name));
        char et[24] = "", am[32] = "";
        bool pin = false;
        for (size_t t = 0; t < r->txt_count; t++) {
            const char *k = r->txt[t].key, *v = r->txt[t].value ? r->txt[t].value : "";
            if (!k) continue;
            if (!strcasecmp(k, "et")) strlcpy(et, v, sizeof(et));
            else if (!strcasecmp(k, "am") || !strcasecmp(k, "model")) strlcpy(am, v, sizeof(am));
            else if (!strcasecmp(k, "pw") && (!strcasecmp(v, "true") || !strcmp(v, "1"))) pin = true;
            else if (!strcasecmp(k, "sf") || !strcasecmp(k, "flags")) {
                uint32_t sf = (uint32_t)strtoul(v, NULL, 0);
                if ((sf & 0x08U) || ((sf & 0x200U) && !app_is_ap2_speaker_paired(ip))) pin = true;
            }
        }
        int idx = -1;
        for (int i = 0; i < *count; i++) if (!strcmp(out[i].ip, ip)) { idx = i; break; }
        gls_ap_speaker_t *s = idx >= 0 ? &out[idx] : &out[(*count)++];
        if (idx < 0) { memset(s, 0, sizeof(*s)); strlcpy(s->name, name, sizeof(s->name)); strlcpy(s->ip, ip, sizeof(s->ip)); }
        if (r->port) s->port = r->port;   // _raop and _airplay both report the RTSP port (7000 on HomePods)
        if (am[0]) strlcpy(s->model, am, sizeof(s->model));
        s->ap2 = s->ap2 || should_use_ap2(am, et);
        s->requires_pin = s->requires_pin || pin;
    }
}

int gls_ap_discover(gls_ap_speaker_t *out, int max, int timeout_ms)
{
    if (gls_ap_init() != ESP_OK) return 0;
    persist_paired();
    int count = 0;
    mdns_result_t *res = NULL;
    if (mdns_query_ptr("_airplay", "_tcp", timeout_ms, 20, &res) == ESP_OK && res) { ingest(res, out, &count, max); mdns_query_results_free(res); }
    res = NULL;
    if (mdns_query_ptr("_raop", "_tcp", timeout_ms / 2, 20, &res) == ESP_OK && res) { ingest(res, out, &count, max); mdns_query_results_free(res); }
    for (int i = 0; i < count; i++) if (!out[i].port) out[i].port = 7000;
    return count;
}

// ---------------------------------------------------------------- live stream
// 16 kHz mono ring (PSRAM, single producer = I2S writer, single consumer = sender task).
#define RING_N (16000 * 2)
#define FEED_TARGET (16000 * 60 / 1000)   // keep ~60 ms queued before playing after an underrun
static int16_t *s_ring;
static volatile uint32_t s_wr, s_rd;
static volatile bool s_stream_run, s_stream_ready, s_flush_req;
static volatile int s_stream_delay_ms;
static uint32_t ring_fill(void) { return s_wr - s_rd; }

// What was actually sent, indexed by RTP time (44.1 kHz mono): the receiver plays RTP time T at
// T + its fixed playout delay, so this is the exact AEC reference, whatever the queue/underruns do.
#define HIST_N 131072u                    // ~3 s, power of two
static int16_t *s_hist;
static volatile uint64_t s_hist_end;      // RTP time after the last recorded frame
static volatile bool s_onset_armed;
static volatile uint64_t s_onset_ts;

static void hist_zero(uint64_t from, uint64_t to)
{
    if (to <= from) return;
    if (to - from > HIST_N) from = to - HIST_N;
    for (uint64_t t = from; t < to; t++) s_hist[t & (HIST_N - 1)] = 0;
}

static void hist_record(uint64_t ts0, const int16_t *stereo, int frames)
{
    if (!s_hist) return;
    uint64_t end = s_hist_end;
    if (end && ts0 > end) hist_zero(end, ts0);             // resync gap: nothing was sent there
    for (int i = 0; i < frames; i++) {
        int16_t v = stereo[2 * i];
        s_hist[(ts0 + i) & (HIST_N - 1)] = v;
        if (s_onset_armed && (v > 3000 || v < -3000)) { s_onset_ts = ts0 + i; s_onset_armed = false; }
    }
    s_hist_end = ts0 + frames;
}

bool gls_ap_ref_read(int64_t t_us, int16_t *out, size_t n)
{
    int play = s_play_ms;
    uint64_t end = s_hist_end;
    if (!s_hist || play <= 0 || !s_stream_ready || !end) return false;
    // frame at 44.1 kHz for output time t_us: RTP(t) - play delay; step 44100/16000 per 16 kHz sample
    uint64_t base = gls_raop_ts_at_us(t_us) - (uint64_t)play * 441 / 10;
    for (size_t k = 0; k < n; k++) {
        uint64_t ts = base + (uint64_t)k * 44100 / 16000;
        out[k] = (ts < end && ts + HIST_N - 8192 > end) ? s_hist[ts & (HIST_N - 1)] : 0;
    }
    return true;
}

void gls_ap_onset_arm(void) { s_onset_ts = 0; s_onset_armed = true; }
int64_t gls_ap_onset_us(void)
{
    // esp_timer time at which the first loud sent frame was due (RTP time -> microseconds)
    uint64_t ts = s_onset_ts;
    if (!ts) return -1;
    int64_t now = esp_timer_get_time();
    int64_t dts = (int64_t)(gls_raop_ts_at_us(now) - ts);
    return now - dts * 1000000 / 44100;
}
void gls_ap_set_play_delay_ms(int ms) { load_settings(); s_play_ms = ms > 0 ? ms : 0; save_u16("play_ms", (uint16_t)s_play_ms); }
int gls_ap_play_delay_ms(void) { load_settings(); return s_play_ms; }
void gls_ap_use_play_delay_ms(int ms) { load_settings(); s_play_ms = ms > 0 ? ms : 0; }  // runtime only (no flash write)

void gls_ap_stream_push(const int16_t *pcm, size_t n)
{
    if (!s_ring || !s_stream_run) return;
    for (size_t i = 0; i < n; i++) {
        if (ring_fill() >= RING_N - 1) s_rd++;        // full: drop oldest
        s_ring[s_wr % RING_N] = pcm ? pcm[i] : 0;
        s_wr++;
    }
}

static void stream_task(void *arg)
{
    AirPlaySpeaker sp = {};
    strlcpy(sp.name, s_out.name, sizeof(sp.name));
    strlcpy(sp.ip, s_out.ip, sizeof(sp.ip));
    sp.port = s_out.port ? s_out.port : 7000;
    strlcpy(sp.am, s_out.model, sizeof(sp.am));
    strlcpy(sp.et, "0,1", sizeof(sp.et));
    strlcpy(sp.cn, "0,1", sizeof(sp.cn));
    sp.is_ap2 = s_out.ap2;
    sp.requires_pin = s_out.requires_pin;
    gls_raop_latency_override = s_latency_ms > 0 ? (uint32_t)s_latency_ms * 441 / 10 : 0;
    RaopClient *c = new RaopClient();
    set_status("call audio: connecting to %s...", sp.name);
    uint64_t t0 = esp_timer_get_time();
    if (!c->connect(sp, s_volume)) {
        set_status("call audio: could not connect to %s (%s)", sp.name, g_app.status_text[0] ? g_app.status_text : "connect failed");
    } else {
        s_client = c;
        s_rd = s_wr;   // start fresh
        s_hist_end = 0;
        load_settings();
        int lat_ms = (int)(c->getLatencyFrames() * 1000ULL / RAOP_SAMPLE_RATE);
        s_stream_delay_ms = lat_ms + FEED_TARGET * 1000 / 16000;
        s_stream_ready = true;
        set_status("call audio -> %s (%s, %d ms delay, connected in %d ms)", sp.name, c->isAirPlay2() ? "AirPlay 2" : "AirPlay 1",
                   s_stream_delay_ms, (int)((esp_timer_get_time() - t0) / 1000));
        static int16_t chunk[RAOP_CHUNK_FRAMES * 2];
        const float step = 16000.0f / RAOP_SAMPLE_RATE;
        float frac = 0;
        bool primed = false;
        uint32_t n_sent = 0, n_loops = 0, max_send_us = 0, busy_us = 0;
        uint64_t stat_t0 = esp_timer_get_time();
        while (s_stream_run) {
            n_loops++;
            uint64_t tb = esp_timer_get_time();
            static uint64_t last_poll;
            if (tb - last_poll >= 20000) {  // control/event/keepalive: 50x per second is plenty
                last_poll = tb;
                c->pollControlAndKeepalive();
            }
            if (s_volume_dirty) { s_volume_dirty = false; c->setVolume(s_volume); }
            if (s_flush_req) {
                s_flush_req = false; s_rd = s_wr; primed = false;
                uint64_t tf = esp_timer_get_time();
                bool ok = c->flushNow();
                if (s_hist && s_play_ms > 0) {  // the receiver drops everything not yet played
                    uint64_t played = gls_raop_ts_at_us(esp_timer_get_time()) - (uint64_t)s_play_ms * 441 / 10;
                    hist_zero(played, s_hist_end);
                }
                ESP_LOGI(TAG, "barge-in: FLUSH -> %s %s (%d ms)", sp.name, ok ? "ok" : "FAILED", (int)((esp_timer_get_time() - tf) / 1000));
            }
            if (esp_timer_get_time() - stat_t0 >= 5000000) {
                ESP_LOGI(TAG, "stream: %u chunks/5s, max send %u us, busy %u%% (encode+crypt %u ms, sendto %u ms max %u us, poll %u ms), queue %u ms",
                         (unsigned)n_sent, (unsigned)max_send_us, (unsigned)(busy_us / 50000),
                         (unsigned)(gls_raop_prof_crypt_us / 1000), (unsigned)(gls_raop_prof_sendto_us / 1000),
                         (unsigned)gls_raop_prof_sendto_max_us, (unsigned)(gls_raop_prof_poll_us / 1000), (unsigned)(ring_fill() / 16));
                gls_raop_prof_crypt_us = gls_raop_prof_sendto_us = gls_raop_prof_poll_us = gls_raop_prof_sendto_max_us = 0;
                n_sent = n_loops = max_send_us = busy_us = 0;
                stat_t0 = esp_timer_get_time();
            }
            if (!c->acceptFrames()) {
                busy_us += (uint32_t)(esp_timer_get_time() - tb);
                vTaskDelay(pdMS_TO_TICKS(3));  // next 352-frame chunk is due every 8 ms
                continue;
            }
            if (!primed && ring_fill() >= FEED_TARGET) primed = true;
            for (int i = 0; i < RAOP_CHUNK_FRAMES; i++) {
                int16_t v = 0;
                if (primed && ring_fill() >= 2) {
                    int16_t a = s_ring[s_rd % RING_N], b = s_ring[(s_rd + 1) % RING_N];
                    v = (int16_t)(a + (b - a) * frac);
                    frac += step;
                    while (frac >= 1.0f && ring_fill() >= 2) { frac -= 1.0f; s_rd++; }
                } else if (primed) {
                    primed = false;  // underrun: wait for FEED_TARGET again (silence meanwhile)
                }
                chunk[2 * i] = chunk[2 * i + 1] = v;
            }
            // keep the queue near its target: drop a little if it grew (clock drift / bursts)
            if (ring_fill() > FEED_TARGET * 4) s_rd = s_wr - FEED_TARGET;
            uint64_t ts = esp_timer_get_time();
            c->sendChunk(chunk);
            hist_record(c->getHeadTs() - RAOP_CHUNK_FRAMES, chunk, RAOP_CHUNK_FRAMES);
            uint32_t dt = (uint32_t)(esp_timer_get_time() - ts);
            if (dt > max_send_us) max_send_us = dt;
            n_sent++;
            busy_us += (uint32_t)(esp_timer_get_time() - tb);
        }
        s_client = NULL;
        s_stream_ready = false;
        c->disconnect();
        set_status("call audio: %s disconnected (%u packets, %u retransmits)", sp.name,
                   (unsigned)c->getPktsSent(), (unsigned)c->getRetransmits());
    }
    delete c;
    s_stream_ready = false;
    s_stream_run = false;
    s_busy = false;
    vTaskDeleteWithCaps(NULL);
}

void gls_ap_set_output(const gls_ap_speaker_t *spk)
{
    load_settings();
    s_out_set = spk && spk->ip[0];
    if (s_out_set) s_out = *spk;
    nvs_handle_t h;
    if (nvs_open("gls_airplay", NVS_READWRITE, &h) == ESP_OK) {
        if (s_out_set) nvs_set_blob(h, "output", &s_out, sizeof(s_out));
        else nvs_erase_key(h, "output");
        nvs_commit(h);
        nvs_close(h);
    }
}

bool gls_ap_output(gls_ap_speaker_t *out)
{
    load_settings();
    if (s_out_set && out) *out = s_out;
    return s_out_set;
}

esp_err_t gls_ap_stream_start(void)
{
    esp_err_t err = gls_ap_init();
    if (err != ESP_OK) return err;
    if (!s_out_set) return ESP_ERR_NOT_FOUND;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (!s_ring) s_ring = (int16_t *)heap_caps_calloc(RING_N, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_hist) s_hist = (int16_t *)heap_caps_calloc(HIST_N, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) return ESP_ERR_NO_MEM;
    s_busy = true;
    s_stream_run = true;
    s_stream_ready = false;
    if (xTaskCreatePinnedToCoreWithCaps(stream_task, "airplay_rx", 24 * 1024, NULL, 12, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy = false;
        s_stream_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void gls_ap_stream_stop(void) { s_stream_run = false; s_stream_ready = false; }
bool gls_ap_stream_ready(void) { return s_stream_ready; }
int gls_ap_stream_delay_ms(void) { return s_stream_delay_ms; }
void gls_ap_stream_flush(void) { if (s_stream_ready) s_flush_req = true; }
int gls_ap_stream_queue_samples(void) { return s_stream_ready ? (int)ring_fill() : 0; }

// ---------------------------------------------------------------- test tone
typedef struct { gls_ap_speaker_t spk; int vol; } tone_job_t;

static void tone_task(void *arg)
{
    tone_job_t *job = (tone_job_t *)arg;
    AirPlaySpeaker sp = {};
    strlcpy(sp.name, job->spk.name, sizeof(sp.name));
    strlcpy(sp.ip, job->spk.ip, sizeof(sp.ip));
    sp.port = job->spk.port ? job->spk.port : 7000;
    strlcpy(sp.am, job->spk.model, sizeof(sp.am));
    strlcpy(sp.et, "0,1", sizeof(sp.et));
    strlcpy(sp.cn, "0,1", sizeof(sp.cn));
    sp.is_ap2 = job->spk.ap2;
    sp.requires_pin = job->spk.requires_pin;
    strlcpy(g_app.pin_speaker_ip, sp.ip, sizeof(g_app.pin_speaker_ip));
    strlcpy(g_app.pin_speaker_name, sp.name, sizeof(g_app.pin_speaker_name));
    g_app.pin_halted_ip[0] = 0;

    RaopClient *c = new RaopClient();
    set_status("connecting to %s (%s)...", sp.name, sp.ip);
    uint64_t t0 = esp_timer_get_time();
    if (!c->connect(sp, s_volume)) {
        char st[96];
        strlcpy(st, g_app.status_text[0] ? g_app.status_text : "connect failed", sizeof(st));
        set_status("failed: %s", st);
    } else {
        set_status("connected to %s (%s) in %d ms, sending 2 s test tone",
                   sp.name, c->isAirPlay2() ? "AirPlay 2" : "AirPlay 1", (int)((esp_timer_get_time() - t0) / 1000));
        s_client = c;
        static int16_t chunk[RAOP_CHUNK_FRAMES * 2];
        const int total = RAOP_SAMPLE_RATE * 2, fade = RAOP_SAMPLE_RATE / 50;
        int n = 0;
        while (n < total + RAOP_SAMPLE_RATE / 2) {  // tone, then 0.5 s silence
            c->pollControlAndKeepalive();
            if (s_volume_dirty) { s_volume_dirty = false; c->setVolume(s_volume); }
            if (!c->acceptFrames()) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
            for (int i = 0; i < RAOP_CHUNK_FRAMES; i++, n++) {
                float v = 0;
                if (n < total) {
                    float env = n < fade ? (float)n / fade : n > total - fade ? (float)(total - n) / fade : 1.0f;
                    v = sinf(2.0f * (float)M_PI * 440.0f * n / RAOP_SAMPLE_RATE) * env * 0.25f * 32767.0f;
                }
                chunk[2 * i] = chunk[2 * i + 1] = (int16_t)v;
            }
            c->sendChunk(chunk);
        }
        // the receiver plays ~2 s behind (buffered AirPlay): keep the session alive until it has played
        uint64_t until = esp_timer_get_time() + 3000000;
        while (esp_timer_get_time() < until) {
            c->pollControlAndKeepalive();
            if (s_volume_dirty) { s_volume_dirty = false; c->setVolume(s_volume); }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        s_client = NULL;
        set_status("done: test tone sent to %s (%u packets, %u retransmits)", sp.name,
                   (unsigned)c->getPktsSent(), (unsigned)c->getRetransmits());
        c->disconnect();
    }
    delete c;
    free(job);
    s_busy = false;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t gls_ap_test_tone(const gls_ap_speaker_t *spk, int volume_pct)
{
    if (!spk || !spk->ip[0]) return ESP_ERR_INVALID_ARG;
    esp_err_t err = gls_ap_init();
    if (err != ESP_OK) return err;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    tone_job_t *job = (tone_job_t *)calloc(1, sizeof(tone_job_t));
    if (!job) return ESP_ERR_NO_MEM;
    job->spk = *spk;
    job->vol = volume_pct;
    s_busy = true;
    // big stack for SRP-3072/Ed25519 pairing; PSRAM (no flash writes on this task, see app_mark_ap2_speaker_paired)
    if (xTaskCreatePinnedToCoreWithCaps(tone_task, "airplay_tx", 24 * 1024, job, 5, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(job);
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void gls_ap_status(char *out, size_t len, bool *busy)
{
    persist_paired();
    if (s_busy && g_app.pin_prompt_active) {
        snprintf(out, len, "PIN required for %s: enter the code shown by the speaker/TV", g_app.pin_speaker_name);
        if (busy) *busy = true;
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    strlcpy(out, s_status, len);
    taskEXIT_CRITICAL(&s_lock);
    if (busy) *busy = s_busy;
}

void gls_ap_set_volume(int pct)
{
    load_settings();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_volume = pct;
    s_volume_dirty = s_client != NULL;  // the sender task applies it on its next loop
    nvs_handle_t h;
    if (nvs_open("gls_airplay", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "volume", (uint8_t)pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

int gls_ap_volume(void) { load_settings(); return s_volume; }

static void save_u16(const char *key, uint16_t v)
{
    nvs_handle_t h;
    if (nvs_open("gls_airplay", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u16(h, key, v); nvs_commit(h); nvs_close(h); }
}
void gls_ap_set_latency_ms(int ms) { load_settings(); s_latency_ms = ms < 0 ? 0 : ms; save_u16("lat_ms", (uint16_t)s_latency_ms); }
int gls_ap_latency_ms(void) { load_settings(); return s_latency_ms; }
void gls_ap_set_measured_delay_ms(int ms) { load_settings(); s_measured_delay_ms = ms < 0 ? 0 : ms; save_u16("meas_ms", (uint16_t)s_measured_delay_ms); }
int gls_ap_measured_delay_ms(void) { load_settings(); return s_measured_delay_ms; }

void gls_ap_submit_pin(const char *pin)
{
    if (!pin) return;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(g_app.pin_code, pin, sizeof(g_app.pin_code));
        g_app.pin_submitted = true;
        g_app.pin_cancelled = false;
        xSemaphoreGive(g_app.mutex);
    }
}
