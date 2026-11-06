#include "rtc_peer.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "media_lib_os.h"
#include "sdkconfig.h"

#include "media.h"
#include "sc_port.h"

static const char *TAG = "rtc_peer";

#define DC_STREAM_ID 0 /* /realtime/wm?dcid=0 */

static struct {
    esp_peer_handle_t    pc;
    rtc_peer_callbacks_t cb;
    atomic_bool          running;      /* pc_task loop */
    atomic_bool          sending;      /* pc_send loop */
    SemaphoreHandle_t    pc_exit;
    SemaphoreHandle_t    send_exit;
    bool                 send_started;
    atomic_bool          dc_open;
    atomic_bool          connected;
    atomic_bool          stopping;
    bool                 dtls_client;  /* from the answer: a=setup:passive -> we are DTLS/SCTP client */
    atomic_uint_least64_t last_remote_audio_ms;
    atomic_uint          remote_packets;
    uint32_t             sent_packets;
} p;

// ---------------------------------------------------------------------------
// SDP helpers
// ---------------------------------------------------------------------------

/** Replace every occurrence of `from` with `to` (same or shorter length not required). */
static char *str_replace_all(const char *s, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to), n = 0;
    for (const char *q = s; (q = strstr(q, from)); q += fl) {
        n++;
    }
    char *out = malloc(strlen(s) + n * (tl > fl ? tl - fl : 0) + 1);
    if (!out) {
        return NULL;
    }
    char *w = out;
    const char *r = s, *q;
    while ((q = strstr(r, from))) {
        memcpy(w, r, q - r);
        w += q - r;
        memcpy(w, to, tl);
        w += tl;
        r = q + fl;
    }
    strcpy(w, r);
    return out;
}

/** Log an SDP without the ICE password. */
static void log_sdp(const char *what, const char *sdp)
{
    ESP_LOGI(TAG, "---- %s (%u B) ----", what, (unsigned)strlen(sdp));
    const char *line = sdp;
    while (*line) {
        const char *end = strpbrk(line, "\r\n");
        int len = end ? (int)(end - line) : (int)strlen(line);
        if (len > 0) {
            if (!strncmp(line, "a=ice-pwd:", 10)) {
                ESP_LOGI(TAG, "a=ice-pwd:<redacted>");
            } else {
                ESP_LOGI(TAG, "%.*s", len, line);
            }
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
}

// ---------------------------------------------------------------------------
// esp_peer callbacks (run in pc_task)
// ---------------------------------------------------------------------------

static void notify(rtc_peer_event_t ev)
{
    if (p.cb.on_event) {
        p.cb.on_event(ev, p.cb.ctx);
    }
}

static void send_frame(const uint8_t *data, int size, uint32_t pts, void *ctx)
{
    esp_peer_audio_frame_t f = {
        .pts = pts,
        .data = (uint8_t *)data,
        .size = size,
    };
    if (p.pc && esp_peer_send_audio(p.pc, &f) == ESP_PEER_ERR_NONE) {
        p.sent_packets++;
    }
}

static void pc_send_task(void *arg)
{
    while (atomic_load(&p.sending)) {
        gls_media_capture_drain(send_frame, NULL);
        media_lib_thread_sleep(10);
    }
    xSemaphoreGive(p.send_exit);
    media_lib_thread_destroy(NULL);
}

static void dc_mark_open(const char *why)
{
    if (!atomic_exchange(&p.dc_open, true)) {
        ESP_LOGI(TAG, "data channel usable on stream %d (%s)", DC_STREAM_ID, why);
        notify(RTC_PEER_EV_DC_OPEN);
    }
}

static int on_state(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "peer state %d", (int)state);
    switch (state) {
    case ESP_PEER_STATE_CONNECTED:
        atomic_store(&p.connected, true);
        if (!p.send_started && gls_media_capture_start() == ESP_OK) {
            atomic_store(&p.sending, true);
            media_lib_thread_handle_t th = NULL;
            if (media_lib_thread_create_from_scheduler(&th, "pc_send", pc_send_task, NULL) == 0 && th) {
                p.send_started = true;
            } else {
                atomic_store(&p.sending, false);
                ESP_LOGE(TAG, "pc_send thread failed");
            }
        }
        notify(RTC_PEER_EV_CONNECTED);
        break;
    case ESP_PEER_STATE_DATA_CHANNEL_CONNECTED:
        // SCTP association is up. /wm only ever uses stream 0 and never ACKs DCEP on it.
#if !CONFIG_GLS_DC_MODE_RAW
        if (p.dtls_client) {
            // As SCTP client the first local channel gets stream 0: this registers stream 0 in
            // esp_peer (needed for sends). Its DCEP OPEN goes out on stream 0 and is ignored.
            esp_peer_data_channel_cfg_t ch = {
                .type = ESP_PEER_DATA_CHANNEL_RELIABLE,
                .ordered = true,
                .label = "oai-events",
            };
            int ret = esp_peer_create_data_channel(p.pc, &ch);
            ESP_LOGI(TAG, "SCTP up: registered local channel for stream 0 (ret %d)", ret);
        } else {
            // As SCTP server esp_peer would pick an ODD stream id: the server resets it and
            // ABORTs the association if we send on it. Do not create a channel.
            ESP_LOGW(TAG, "SCTP up as server role: not creating a channel (would be an odd stream)");
        }
#else
        ESP_LOGI(TAG, "SCTP up: raw stream 0, no local channel");
#endif
#if !CONFIG_GLS_DC_MODE_DCEP_ACK
        dc_mark_open("SCTP up");
#endif
        break;
    case ESP_PEER_STATE_DATA_CHANNEL_OPENED:
        ESP_LOGI(TAG, "data channel opened");
        break;
    case ESP_PEER_STATE_CLOSED:
        atomic_store(&p.dc_open, false);
        if (atomic_exchange(&p.connected, false) && !atomic_load(&p.stopping))
            notify(RTC_PEER_EV_REMOTE_CLOSED);
        break;
    case ESP_PEER_STATE_DISCONNECTED:
        if (atomic_exchange(&p.connected, false)) {
            notify(RTC_PEER_EV_DISCONNECTED);
        }
        break;
    case ESP_PEER_STATE_CONNECT_FAILED:
        notify(RTC_PEER_EV_FAILED);
        break;
    default:
        break;
    }
    return 0;
}

static int on_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (msg->type != ESP_PEER_MSG_TYPE_SDP || !msg->data) {
        return 0;
    }
    char *sdp = strndup((const char *)msg->data, msg->size > 0 ? (size_t)msg->size : strlen((char *)msg->data));
    if (!sdp) {
        return -1;
    }
#if CONFIG_GLS_OFFER_DTLS_ACTIVE
    // Make the device the DTLS client -> SCTP client -> even stream ids, so the locally
    // registered channel is stream 0 = dcid (the only stream /wm uses).
    char *rewritten = str_replace_all(sdp, "a=setup:actpass", "a=setup:active");
    if (rewritten) {
        free(sdp);
        sdp = rewritten;
    }
#endif
    log_sdp("local offer", sdp);
    if (p.cb.on_offer) {
        p.cb.on_offer(sdp, p.cb.ctx); // ownership passes on
    } else {
        free(sdp);
    }
    return 0;
}

static int on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx)
{
    // The answer says opus/48000/2; esp_peer reports what was negotiated. av_render decodes and
    // resamples/downmixes to the fixed 16 kHz mono render format.
    ESP_LOGI(TAG, "remote audio: codec %d, %d Hz, %d ch", info->codec, (int)info->sample_rate, info->channel);
    gls_media_play_stream(info->sample_rate, info->channel);
    return 0;
}

// Downlink packet statistics (logged every 5 s while audio flows): lost = pts jumps,
// late = arrival gaps > 60 ms inside a burst. Tells network loss from scheduling stalls.
static struct {
    uint32_t last_pts, step, pkts, lost, late, max_gap_ms;
    uint64_t last_rx_ms, last_log_ms;
} s_rx;

static void rx_stats(uint32_t pts)
{
    uint64_t now = gls_now_ms();
    if (s_rx.pkts) {
        uint32_t d = pts - s_rx.last_pts;
        if (d && (!s_rx.step || d < s_rx.step)) s_rx.step = d; // smallest step = one frame
        if (s_rx.step && d > s_rx.step + s_rx.step / 2 && d < s_rx.step * 50) s_rx.lost += d / s_rx.step - 1;
        uint32_t gap = (uint32_t)(now - s_rx.last_rx_ms);
        if (gap < 1000) {
            if (gap > 60) s_rx.late++;
            if (gap > s_rx.max_gap_ms) s_rx.max_gap_ms = gap;
        }
    }
    s_rx.pkts++;
    s_rx.last_pts = pts;
    s_rx.last_rx_ms = now;
    if (now - s_rx.last_log_ms >= 5000) {
        ESP_LOGI(TAG, "rx audio: %u pkts, lost %u, late(>60ms) %u, max gap %u ms (step %u); playout underruns total %u, buffer %u ms, stretched %u, dropped %u",
                 (unsigned)s_rx.pkts, (unsigned)s_rx.lost, (unsigned)s_rx.late, (unsigned)s_rx.max_gap_ms,
                 (unsigned)s_rx.step, (unsigned)gls_media_underruns(), (unsigned)gls_media_buffer_ms(),
                 (unsigned)gls_media_stretched(), (unsigned)gls_media_dropped());
        s_rx.pkts = s_rx.lost = s_rx.late = s_rx.max_gap_ms = 0;
        s_rx.last_log_ms = now;
    }
}

static int on_audio_data(esp_peer_audio_frame_t *frame, void *ctx)
{
    rx_stats(frame->pts);
    atomic_store(&p.last_remote_audio_ms, gls_now_ms());
    atomic_fetch_add(&p.remote_packets, 1);
    gls_media_play_opus(frame->data, frame->size, frame->pts);
    return 0;
}

static int on_video_info(esp_peer_video_stream_info_t *info, void *ctx)
{
    return 0;
}

static int on_video_data(esp_peer_video_frame_t *frame, void *ctx)
{
    return 0;
}

static int on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
    ESP_LOGI(TAG, "data channel '%s' open event on stream %u", ch->label ? ch->label : "", ch->stream_id);
    if (ch->stream_id != DC_STREAM_ID) {
        // Never use an odd/other stream: the server resets it and aborts SCTP on data.
        ESP_LOGE(TAG, "ignoring channel on stream %u: we only ever send on stream %d", ch->stream_id, DC_STREAM_ID);
        return 0;
    }
    dc_mark_open("DCEP open/ACK");
    return 0;
}

static int on_channel_close(esp_peer_data_channel_info_t *ch, void *ctx)
{
    ESP_LOGW(TAG, "data channel closed (stream %u)", ch->stream_id);
    if (ch->stream_id != DC_STREAM_ID) return 0; // ignored streams cannot close stream 0
    if (atomic_exchange(&p.dc_open, false) && !atomic_load(&p.stopping)) {
        atomic_store(&p.connected, false);
        atomic_store(&p.sending, false);
        notify(RTC_PEER_EV_REMOTE_CLOSED);
    }
    return 0;
}

static int on_data(esp_peer_data_frame_t *frame, void *ctx)
{
    if (frame->stream_id != DC_STREAM_ID) {
        ESP_LOGW(TAG, "dropping %d B on unexpected stream %u", frame->size, frame->stream_id);
        return 0;
    }
    dc_mark_open("first stream-0 message");
    if (p.cb.on_data && frame->data && frame->size > 0) {
        p.cb.on_data(frame->data, frame->size, p.cb.ctx);
    }
    return 0;
}

static void pc_task(void *arg)
{
    while (atomic_load(&p.running)) {
        esp_peer_main_loop(p.pc);
        media_lib_thread_sleep(10);
    }
    xSemaphoreGive(p.pc_exit);
    media_lib_thread_destroy(NULL);
}

// ---------------------------------------------------------------------------

esp_err_t rtc_peer_start(const rtc_peer_callbacks_t *cb)
{
    if (p.pc) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!p.pc_exit) {
        p.pc_exit = xSemaphoreCreateBinary();
        p.send_exit = xSemaphoreCreateBinary();
    }
    atomic_store(&p.stopping, false);
    p.cb = *cb;
    p.send_started = false;
    p.dtls_client = false;
    p.sent_packets = 0;
    atomic_store(&p.dc_open, false);
    atomic_store(&p.connected, false);
    atomic_store(&p.last_remote_audio_ms, 0);
    atomic_store(&p.remote_packets, 0);

    esp_peer_default_cfg_t dcfg = {
        .agent_recv_timeout = 500,
        // Mirrors esp-webrtc-solution's openai_demo, which talks to the same "realtimeapi" stack.
        .ice_use_lite_mode = CONFIG_GLS_PEER_ICE_LITE_FLAG,
        .tcp_support = false, // server also offers TCP 443 passive candidates; UDP 3478 first
        .alive_binding_retries = 0,
    };
    esp_peer_cfg_t cfg = {
        .server_lists = NULL, // no STUN/TURN: server learns our mapping as peer-reflexive
        .server_num = 0,
        .role = ESP_PEER_ROLE_CONTROLLING, // remote is ICE-lite
        .audio_info = {
            .codec = ESP_PEER_AUDIO_CODEC_OPUS,
            .sample_rate = 16000, // we encode 16 kHz mono; SDP still says opus/48000/2 (RTP clock)
            .channel = 1,
        },
        .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
        .video_dir = ESP_PEER_MEDIA_DIR_NONE, // fake-speaker: no video m-line needed
        .enable_data_channel = true,
        .manual_ch_create = true, // never auto-DCEP; DCEP (if enabled) is sent by us on SCTP up
        .no_auto_reconnect = true, // the FSM decides about reconnects
        .on_state = on_state,
        .on_msg = on_msg,
        .on_video_info = on_video_info,
        .on_audio_info = on_audio_info,
        .on_video_data = on_video_data,
        .on_audio_data = on_audio_data,
        .on_channel_open = on_channel_open,
        .on_data = on_data,
        .on_channel_close = on_channel_close,
        .extra_cfg = &dcfg,
        .extra_size = sizeof(dcfg),
    };
    int ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &p.pc);
    if (ret != ESP_PEER_ERR_NONE || !p.pc) {
        ESP_LOGE(TAG, "esp_peer_open failed (%d)", ret);
        p.pc = NULL;
        return ESP_FAIL;
    }
    atomic_store(&p.running, true);
    media_lib_thread_handle_t th = NULL;
    if (media_lib_thread_create_from_scheduler(&th, "pc_task", pc_task, NULL) != 0 || !th) {
        atomic_store(&p.running, false);
        esp_peer_close(p.pc);
        p.pc = NULL;
        return ESP_FAIL;
    }
    ret = esp_peer_new_connection(p.pc); // gathers candidates, then on_msg(offer)
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_new_connection failed (%d)", ret);
        rtc_peer_stop();
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t rtc_peer_apply_answer(const char *sdp)
{
    if (!p.pc || !sdp) {
        return ESP_ERR_INVALID_STATE;
    }
    log_sdp("remote answer", sdp);
    // Our offer said active (or actpass): the answer's passive means we are the DTLS client.
    p.dtls_client = strstr(sdp, "a=setup:passive") != NULL;
    ESP_LOGI(TAG, "DTLS/SCTP role: %s", p.dtls_client ? "client (stream 0 is ours to register)" : "server");
    esp_peer_msg_t msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)sdp,
        .size = (int)strlen(sdp),
    };
    int ret = esp_peer_send_msg(p.pc, &msg);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "applying answer failed (%d)", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t rtc_peer_send_text(const char *text)
{
    if (!p.pc || !atomic_load(&p.dc_open)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_peer_data_frame_t f = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = DC_STREAM_ID, /* never anything else: odd streams make the server ABORT SCTP */
        .data = (uint8_t *)text,
        .size = (int)strlen(text),
    };
    int ret = esp_peer_send_data(p.pc, &f);
    if (ret != ESP_PEER_ERR_NONE) {
        // e.g. "Stream not found 0" in RAW mode, or esp_peer gating sends on a DCEP ACK.
        ESP_LOGW(TAG, "send on stream %d failed (%d)", DC_STREAM_ID, ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void rtc_peer_stop(void)
{
    if (!p.pc) {
        return;
    }
    atomic_store(&p.stopping, true);
    atomic_store(&p.connected, false);
    atomic_store(&p.dc_open, false);
    ESP_LOGI(TAG, "stopping (sent %u audio packets, received %u)", (unsigned)p.sent_packets,
             (unsigned)atomic_load(&p.remote_packets));
    if (p.send_started) {
        atomic_store(&p.sending, false);
        xSemaphoreTake(p.send_exit, pdMS_TO_TICKS(2000));
        p.send_started = false;
    }
    gls_media_capture_stop();
    esp_peer_disconnect(p.pc);
    atomic_store(&p.running, false);
    xSemaphoreTake(p.pc_exit, pdMS_TO_TICKS(3000));
    esp_peer_close(p.pc);
    p.pc = NULL;
    atomic_store(&p.dc_open, false);
    atomic_store(&p.connected, false);
    gls_media_play_reset();
    atomic_store(&p.stopping, false);
}

bool rtc_peer_active(void)
{
    return atomic_load(&p.running) || atomic_load(&p.stopping);
}

bool rtc_peer_connected(void)
{
    return atomic_load(&p.connected) && atomic_load(&p.dc_open) && !atomic_load(&p.stopping);
}

uint64_t rtc_peer_last_remote_audio_ms(void)
{
    return atomic_load(&p.last_remote_audio_ms);
}

uint32_t rtc_peer_remote_audio_packets(void)
{
    return atomic_load(&p.remote_packets);
}
