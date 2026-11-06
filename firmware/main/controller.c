#include "controller.h"
#include "airplay_tx.h"
#include "chime.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "nvs.h"
#include "voice_control.h"

#include "chatgpt_auth.h"
#include "chatgpt_wm.h"
#include "led.h"
#include "media.h"
#include "rtc_peer.h"
#include "sc_port.h"
#include "session_fsm.h"
#include "wm_events.h"
#include "wifi.h"
#include "xvf3800_i2s.h"

static const char *TAG = "ctrl";

#define TICK_MS              1000
#define POLL_MS              200   /* queue wait; also the remote-speaking detector period */
#define REMOTE_QUIET_MS      1500  /* no remote audio/caption for this long -> not speaking */
#define OFFER_TIMEOUT_MS     10000

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

typedef enum {
    MSG_INPUT,   /* ready-made FSM input (now_ms filled in by the controller) */
    MSG_DC_DATA, /* data-channel message: data (malloc'd), len */
} msg_type_t;

typedef struct {
    msg_type_t     type;
    sc_fsm_input_t in;
    char          *data;
    int            len;
} ctrl_msg_t;

typedef enum {
    JOB_AUTH,
    JOB_CALL,
    JOB_HANGUP,
    JOB_CLEAR,
    JOB_SET_CREDS,
} job_type_t;

typedef struct {
    job_type_t type;
    uint32_t   seq;
    bool       force;
    bool       rotation;
    char       conversation_id[SC_WM_ID_MAX];
    char      *chunk0, *chunk1; /* JOB_SET_CREDS, malloc'd */
    QueueHandle_t reply;
} job_t;

static QueueHandle_t s_q;       /* -> controller */
static QueueHandle_t s_jobs;    /* -> worker */
static QueueHandle_t s_offer_q; /* rtc_peer offer -> worker (char *) */
static atomic_uint s_call_seq;  /* seq of the call the peer belongs to */
static bool s_started;
static atomic_bool s_assistant_speaking;
static atomic_uint s_barge_ins;
static gls_controller_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

static void update_auth_status(const char *error)
{
    const chatgpt_token_t *tok = chatgpt_session_token();
    portENTER_CRITICAL(&s_status_lock);
    s_status.has_credentials = chatgpt_session_has_credentials();
    s_status.logged_in = tok && !error;
    s_status.token_exp = tok ? tok->exp_unix : 0;
    strlcpy(s_status.email, tok ? tok->email : "", sizeof(s_status.email));
    strlcpy(s_status.last_error, error ? error : "", sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_status_lock);
}

static void post(const ctrl_msg_t *m)
{
    if (!s_q || xQueueSend(s_q, m, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "controller queue full, dropping msg %d/%d", m->type, m->in.type);
        if (m->type == MSG_DC_DATA) {
            free(m->data);
        }
    }
}

static void post_input(sc_fsm_input_type_t type, uint32_t seq)
{
    ctrl_msg_t m = {.type = MSG_INPUT};
    m.in.type = type;
    m.in.seq = seq;
    post(&m);
}

static void post_job(const job_t *j)
{
    if (xQueueSend(s_jobs, j, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "worker queue full, dropping job %d", j->type);
    }
}

// ---------------------------------------------------------------------------
// Peer callbacks (run in esp_peer's pc_task): copy and forward
// ---------------------------------------------------------------------------

static void peer_on_offer(char *sdp, void *ctx)
{
    if (xQueueSend(s_offer_q, &sdp, 0) != pdTRUE) {
        free(sdp);
    }
}

static void peer_on_event(rtc_peer_event_t ev, void *ctx)
{
    uint32_t seq = atomic_load(&s_call_seq);
    switch (ev) {
    case RTC_PEER_EV_CONNECTED:
#if CONFIG_GLS_PEER_CONNECTED_ON_DTLS
        post_input(SC_IN_PEER_CONNECTED, seq);
#endif
        break;
    case RTC_PEER_EV_DC_OPEN:
#if !CONFIG_GLS_PEER_CONNECTED_ON_DTLS
        post_input(SC_IN_PEER_CONNECTED, seq);
#endif
        break;
    case RTC_PEER_EV_DISCONNECTED:
        post_input(SC_IN_PEER_CLOSED, seq);
        break;
    case RTC_PEER_EV_REMOTE_CLOSED:
        post_input(SC_IN_REMOTE_CLOSE, seq);
        break;
    case RTC_PEER_EV_FAILED:
        post_input(SC_IN_PEER_FAILED, seq);
        break;
    }
}

static void peer_on_data(const uint8_t *data, int len, void *ctx)
{
    ctrl_msg_t m = {.type = MSG_DC_DATA, .len = len};
    m.in.seq = atomic_load(&s_call_seq);
    m.data = malloc(len + 1);
    if (!m.data) {
        return;
    }
    memcpy(m.data, data, len);
    m.data[len] = '\0';
    post(&m);
}

// ---------------------------------------------------------------------------
// Worker: blocking HTTP + peer setup/teardown, in order
// ---------------------------------------------------------------------------

static void do_call(const job_t *j)
{
    ctrl_msg_t res = {.type = MSG_INPUT};
    res.in.type = SC_IN_SIGNAL_FAIL;
    res.in.seq = j->seq;
    res.in.fail = SC_FAIL_NETWORK;

    const chatgpt_token_t *tok = chatgpt_session_token();
    if (!tok) {
        res.in.fail = SC_FAIL_UNAUTHORIZED;
        post(&res);
        return;
    }
    rtc_peer_stop();
    char *stale;
    while (xQueueReceive(s_offer_q, &stale, 0) == pdTRUE) {
        free(stale);
    }
    atomic_store(&s_call_seq, j->seq);
    rtc_peer_callbacks_t cb = {
        .on_offer = peer_on_offer,
        .on_event = peer_on_event,
        .on_data = peer_on_data,
    };
    uint64_t t0 = gls_now_ms();
    if (rtc_peer_start(&cb) != ESP_OK) {
        res.in.fail = SC_FAIL_OTHER;
        post(&res);
        return;
    }
    char *offer = NULL;
    if (xQueueReceive(s_offer_q, &offer, pdMS_TO_TICKS(OFFER_TIMEOUT_MS)) != pdTRUE || !offer) {
        ESP_LOGE(TAG, "no local offer within %d ms", OFFER_TIMEOUT_MS);
        rtc_peer_stop();
        post(&res);
        return;
    }
    uint64_t t_offer = gls_now_ms();
    chatgpt_wm_request_t req = {
        .access_token = tok->access_token,
        .account_id = tok->account_id,
        .device_id = chatgpt_session_device_id(),
        .offer_sdp = offer,
        .conversation_id = j->rotation && j->conversation_id[0] ? j->conversation_id : NULL,
    };
    chatgpt_wm_result_t wm;
    esp_err_t err = chatgpt_wm_create_call(&req, &wm);
    free(offer);
    if (err != ESP_OK) {
        res.in.fail = wm.fail;
        chatgpt_wm_result_free(&wm);
        rtc_peer_stop();
        post(&res);
        return;
    }
    uint64_t t_answer = gls_now_ms();
    err = rtc_peer_apply_answer(wm.answer_sdp);
    chatgpt_wm_result_free(&wm);
    if (err != ESP_OK) {
        res.in.fail = SC_FAIL_OTHER;
        rtc_peer_stop();
        post(&res);
        return;
    }
    ESP_LOGI(TAG, "call %u signaled: offer %llu ms, /realtime/wm %llu ms", (unsigned)j->seq,
             (unsigned long long)(t_offer - t0), (unsigned long long)(t_answer - t_offer));
    res.in.type = SC_IN_SIGNAL_OK;
    post(&res);
}

static void worker_task(void *arg)
{
    job_t j;
    for (;;) {
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (j.type) {
        case JOB_AUTH: {
            ctrl_msg_t res = {.type = MSG_INPUT};
            chatgpt_session_authenticate(j.force, &res.in);
            update_auth_status(res.in.type == SC_IN_AUTH_OK ? NULL :
                               res.in.fail == SC_FAIL_UNAUTHORIZED ? "Session expired; sign in again" : "ChatGPT authentication failed");
            res.in.seq = j.seq;
            post(&res);
            break;
        }
        case JOB_CALL:
            do_call(&j);
            break;
        case JOB_HANGUP:
            rtc_peer_stop();
            break;
        case JOB_CLEAR:
            rtc_peer_stop();
            chatgpt_session_clear();
            update_auth_status(NULL);
            break;
        case JOB_SET_CREDS: {
            esp_err_t err = chatgpt_session_set_credentials(j.chunk0, j.chunk1);
            if (err == ESP_OK) {
                update_auth_status(NULL);
                post_input(SC_IN_CREDENTIALS_SET, 0);
            }
            if (j.chunk0) memset(j.chunk0, 0, strlen(j.chunk0));
            if (j.chunk1) memset(j.chunk1, 0, strlen(j.chunk1));
            free(j.chunk0);
            free(j.chunk1);
            if (j.reply) xQueueSend(j.reply, &err, portMAX_DELAY);
            break;
        }
        }
    }
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

static struct {
    sc_fsm_t        fsm;
    sc_wm_tracker_t tracker;
    sc_wm_event_t   ev; /* ~800 B, keep off the stack */
    char            conversation_id[SC_WM_ID_MAX];
    sc_rt_state_t   last_explicit_state;
    bool            synth_speaking;
    uint64_t        last_caption_ms;
    sc_fsm_state_t  last_state;
    sc_led_t        last_led;
    char            user_command[SC_WM_TEXT_MAX];
    int             command_index;
    bool            command_truncated;
    sc_voice_stop_t transcript_stop, delta_stop;
} c;

static void execute(const sc_fsm_output_t *o)
{
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.state, sc_fsm_state_name(o->state), sizeof(s_status.state));
    s_status.muted = c.fsm.muted;
    strlcpy(s_status.remote_state, sc_rt_state_name(c.fsm.remote_state), sizeof(s_status.remote_state));
    if (o->actions & SC_ACT_HANG_UP) {
        s_status.last_hangup_reason = o->hangup_reason;
        s_status.last_hangup_ms = gls_now_ms();
    }
    portEXIT_CRITICAL(&s_status_lock);
    if (o->state != c.last_state) {
        c.user_command[0] = 0;
        c.command_index = -1;
        memset(&c.transcript_stop, 0, sizeof(c.transcript_stop));
        memset(&c.delta_stop, 0, sizeof(c.delta_stop));
        ESP_LOGI(TAG, "state %s -> %s (internal free %u, largest %u, min %u; dma free %u)",
                 sc_fsm_state_name(c.last_state), sc_fsm_state_name(o->state),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
        // Audible call state: rising chime when the call is live, falling chime when it ends.
        // AirPlay output: connect while the call is being set up (~2 s), drop it when the call ends
        if (o->state == SC_ST_SIGNALING && gls_ap_output(NULL)) gls_ap_stream_start();
        // keep it through retries (backoff -> signaling); only drop it once the speaker is at rest again
        if (o->state != SC_ST_SIGNALING && o->state != SC_ST_CONNECTING && o->state != SC_ST_IN_CALL &&
            o->state != SC_ST_BACKOFF) gls_ap_stream_stop();
        if (o->state == SC_ST_IN_CALL) {
            gls_chime_play(GLS_CHIME_CONNECTED);
        } else if (c.last_state == SC_ST_IN_CALL) {
            gls_chime_play(GLS_CHIME_DISCONNECTED);
        }
        c.last_state = o->state;
        if (o->state != SC_ST_IN_CALL) {
            c.synth_speaking = false;
        }
    }
    if (o->led != c.last_led) {
        gls_led_set(o->led);
        c.last_led = o->led;
    }
    atomic_store(&s_assistant_speaking, o->state == SC_ST_IN_CALL && c.fsm.remote_state == SC_RT_SPEAKING);
    uint32_t a = o->actions;
    if (!a) {
        return;
    }
    // Order matters: tear down before starting anything new.
    if (a & SC_ACT_HANG_UP) {
        ESP_LOGI(TAG, "hang up (reason %d)", o->hangup_reason);
        job_t j = {.type = JOB_HANGUP};
        post_job(&j);
    }
    if (a & SC_ACT_CLEAR_CREDENTIALS) {
        job_t j = {.type = JOB_CLEAR};
        post_job(&j);
    }
    if (a & SC_ACT_START_AUTH) {
        ESP_LOGI(TAG, "auth #%u%s", (unsigned)o->seq, o->auth_force ? " (forced)" : "");
        job_t j = {.type = JOB_AUTH, .seq = o->seq, .force = o->auth_force};
        post_job(&j);
    }
    if (a & SC_ACT_START_CALL) {
        ESP_LOGI(TAG, "call #%u%s", (unsigned)o->seq, o->call_is_rotation ? " (rotation)" : "");
        job_t j = {.type = JOB_CALL, .seq = o->seq, .rotation = o->call_is_rotation};
        strlcpy(j.conversation_id, c.conversation_id, sizeof(j.conversation_id));
        post_job(&j);
    }
    if (a & SC_ACT_STOP_SPEAKING) {
        char *msg = sc_wm_encode_action("stop_speaking");
        if (msg) {
            esp_err_t err = rtc_peer_send_text(msg);
            ESP_LOGI(TAG, "stop_speaking sent: %s", esp_err_to_name(err));
            free(msg);
        }
        gls_media_play_suppress(350); // silence late packets while server processes stop_speaking
        gls_media_play_flush(); // cut local playback immediately
    }
    if (a & SC_ACT_SET_MIC_MUTE) {
        memset(&c.transcript_stop, 0, sizeof(c.transcript_stop));
        memset(&c.delta_stop, 0, sizeof(c.delta_stop));
        c.user_command[0] = 0;
        c.command_index = -1;
        xvf_i2s_set_mic_mute(true);
    }
    if (a & SC_ACT_SET_MIC_UNMUTE) {
        xvf_i2s_set_mic_mute(false);
    }
    if (a & (SC_ACT_SET_MIC_MUTE | SC_ACT_SET_MIC_UNMUTE)) {
        nvs_handle_t h;
        if (nvs_open("gls_settings", NVS_READWRITE, &h) == ESP_OK) {
            esp_err_t err = nvs_set_u8(h, "muted", c.fsm.muted);
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
            if (err != ESP_OK) ESP_LOGE(TAG, "could not persist microphone mute");
        }
    }
    if (a & SC_ACT_SERVE_SETUP) {
        ESP_LOGW(TAG, "NEEDS LOGIN: open the speaker setup page to import your ChatGPT session");
    }
    if (a & SC_ACT_SCHEDULE_RETRY) {
        ESP_LOGW(TAG, "retry in %u ms", (unsigned)o->retry_delay_ms);
    }
}

static void feed(sc_fsm_input_t *in)
{
    in->now_ms = gls_now_ms();
    sc_fsm_output_t out;
    sc_fsm_handle(&c.fsm, in, &out);
    execute(&out);
}

static void feed_simple(sc_fsm_input_type_t type)
{
    sc_fsm_input_t in = {.type = type};
    feed(&in);
}

static void feed_remote_state(sc_rt_state_t st)
{
    sc_fsm_input_t in = {.type = SC_IN_REMOTE_STATE, .remote_state = st};
    feed(&in);
}

/** Remote audio packets / assistant captions count as "remote speaking": state_update speaking
 *  does not arrive for relay turns (fake-speaker finding). */
static void update_remote_speaking(void)
{
    if (c.fsm.state != SC_ST_IN_CALL) {
        return;
    }
    uint64_t now = gls_now_ms();
    uint64_t last_audio = rtc_peer_last_remote_audio_ms();
    uint64_t last = last_audio > c.last_caption_ms ? last_audio : c.last_caption_ms;
    bool active = last && now - last < REMOTE_QUIET_MS;
    if (active && !c.synth_speaking) {
        c.synth_speaking = true;
        if (c.fsm.remote_state != SC_RT_SPEAKING) {
            feed_remote_state(SC_RT_SPEAKING);
        }
    } else if (!active && c.synth_speaking) {
        c.synth_speaking = false;
        if (c.last_explicit_state != SC_RT_SPEAKING && c.fsm.remote_state == SC_RT_SPEAKING) {
            feed_remote_state(c.last_explicit_state != SC_RT_UNKNOWN ? c.last_explicit_state : SC_RT_LISTENING);
        }
    }
}

static void handle_dc(const char *data, int len)
{
    sc_wm_type_t t = sc_wm_decode(data, (size_t)len, &c.tracker, &c.ev);
    sc_wm_event_t *ev = &c.ev;
    if (ev->conversation_id[0] && strcmp(ev->conversation_id, c.conversation_id) != 0) {
        strlcpy(c.conversation_id, ev->conversation_id, sizeof(c.conversation_id));
        ESP_LOGI(TAG, "conversation %s", c.conversation_id);
    }
    // Only user transcripts can issue a command. Accumulate text patches so split
    // "Seneca" / " stop" updates work without matching assistant or tool text.
    const char *command = NULL;
    if (t == SC_WM_USER_TRANSCRIPTION && !ev->msg.text_truncated) {
        command = ev->msg.text;
    } else if (t == SC_WM_CHAT_MESSAGE_DELTA && ev->msg.role == SC_ROLE_USER) {
        if (ev->msg.is_add || ev->msg.text_op == SC_TEXT_SET || ev->msg.index != c.command_index) {
            c.user_command[0] = 0;
            c.command_truncated = false;
            c.command_index = ev->msg.index;
        }
        if (ev->msg.text_op != SC_TEXT_NONE) {
            if (strlcat(c.user_command, ev->msg.text, sizeof(c.user_command)) >= sizeof(c.user_command))
                c.command_truncated = true;
            c.command_truncated |= ev->msg.text_truncated;
        }
        if (!c.command_truncated) command = c.user_command;
    }
    if (command && c.fsm.state == SC_ST_IN_CALL && !c.fsm.muted) {
        portENTER_CRITICAL(&s_status_lock);
        strlcpy(s_status.last_user_text, command, sizeof(s_status.last_user_text));
        s_status.last_user_text_ms = gls_now_ms();
        portEXIT_CRITICAL(&s_status_lock);
        sc_voice_stop_update(t == SC_WM_USER_TRANSCRIPTION ? &c.transcript_stop : &c.delta_stop,
                             command, gls_now_ms());
    }
    switch (t) {
    case SC_WM_STATE_UPDATE:
        ESP_LOGI(TAG, "remote state %s -> %s", sc_rt_state_name(ev->previous_state), sc_rt_state_name(ev->new_state));
        c.last_explicit_state = ev->new_state;
        feed_remote_state(ev->new_state);
        break;
    case SC_WM_GOODBYE:
    case SC_WM_CLOSE_READY:
    case SC_WM_CLOSE_REQUEST:
        ESP_LOGI(TAG, "remote close (%s)", ev->type_name);
        feed_simple(SC_IN_REMOTE_CLOSE);
        break;
    case SC_WM_USAGE_UPDATE: {
        sc_fsm_input_t in = {
            .type = SC_IN_USAGE,
            .has_session_remaining = ev->has_session_remaining,
            .session_remaining_s = ev->session_remaining_s,
            .hang_up = ev->hang_up,
            .reconnect = ev->reconnect,
        };
        ESP_LOGI(TAG, "usage: session %.0f s left, audio %.0f s left%s%s", ev->session_remaining_s,
                 ev->audio_remaining_s, ev->hang_up ? ", hang_up" : "", ev->reconnect ? ", reconnect" : "");
        feed(&in);
        break;
    }
    case SC_WM_SPAWN_UPDATE:
        if (ev->spawn_start || ev->spawn_end) {
            sc_fsm_input_t in = {.type = SC_IN_SPAWN, .spawn_start = ev->spawn_start};
            ESP_LOGI(TAG, "background %s %s", ev->spawn_kind, ev->spawn_start ? "start" : "end");
            feed(&in);
        }
        break;
    case SC_WM_USER_TRANSCRIPTION:
        ESP_LOGI(TAG, "you: %s", ev->msg.text);
        feed_simple(SC_IN_ACTIVITY);
        break;
    case SC_WM_LIVE_CAPTION:
        c.last_caption_ms = gls_now_ms();
        update_remote_speaking();
        break;
    case SC_WM_CHAT_MESSAGE_DELTA:
        if (ev->msg.role == SC_ROLE_USER && ev->msg.text[0]) {
            ESP_LOGI(TAG, "you: %s", ev->msg.text);
            feed_simple(SC_IN_ACTIVITY);
        } else if (ev->msg.role == SC_ROLE_ASSISTANT && ev->msg.text[0]) {
            if (ev->msg.direction_out) {
                c.last_caption_ms = gls_now_ms(); // spoken assistant text streaming
                update_remote_speaking();
            }
            ESP_LOGI(TAG, "gpt%s: %s", ev->msg.text_op == SC_TEXT_APPEND ? "+" : "", ev->msg.text);
        } else if (ev->msg.role == SC_ROLE_TOOL) {
            ESP_LOGI(TAG, "tool %s", ev->msg.author_name);
            feed_simple(SC_IN_ACTIVITY);
        }
        break;
    case SC_WM_ACTION_REQUEST:
        ESP_LOGI(TAG, "remote action_request %s", ev->action);
        break;
    case SC_WM_INVALID:
        ESP_LOGW(TAG, "unparseable data-channel message (%d B)", len);
        break;
    default:
        ESP_LOGD(TAG, "dc event %s", ev->type_name);
        break;
    }
}

static void controller_task(void *arg)
{
    uint64_t next_tick = gls_now_ms() + TICK_MS;
    ctrl_msg_t m;
    for (;;) {
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(POLL_MS)) == pdTRUE) {
            if (m.type == MSG_INPUT) {
                if (m.in.type == SC_IN_AUTH_FAIL || m.in.type == SC_IN_SIGNAL_FAIL) {
                    ESP_LOGW(TAG, "%s #%u fail class %d", m.in.type == SC_IN_AUTH_FAIL ? "auth" : "signal",
                             (unsigned)m.in.seq, m.in.fail);
                }
                feed(&m.in);
            } else if (m.type == MSG_DC_DATA) {
                if (m.in.seq == atomic_load(&s_call_seq) &&
                    (c.fsm.state == SC_ST_IN_CALL || c.fsm.state == SC_ST_CONNECTING))
                    handle_dc(m.data, m.len);
                free(m.data);
            }
        }
        update_remote_speaking();
        uint64_t now = gls_now_ms();
        if (c.fsm.state == SC_ST_IN_CALL && !c.fsm.muted &&
            (sc_voice_stop_poll(&c.transcript_stop, now) || sc_voice_stop_poll(&c.delta_stop, now))) {
            ESP_LOGI(TAG, "voice hang-up: Seneca stop");
            feed_simple(SC_IN_HANGUP_REQUEST);
        }
        if (now >= next_tick) {
            next_tick = now + TICK_MS;
            feed_simple(SC_IN_TICK);
        }
    }
}

// ---------------------------------------------------------------------------
// BOOT button: short press = WAKE (or stop speaking in a call), 1-4 s = hang up, >= 4 s = mute
// ---------------------------------------------------------------------------

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_GLS_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held_ms = 0;
    for (;;) {
        bool down = gpio_get_level(CONFIG_GLS_BUTTON_GPIO) == 0;
        if (down) {
            held_ms += 20;
        } else if (held_ms >= 40) {
            sc_fsm_input_type_t t = held_ms >= 4000 ? SC_IN_MUTE_TOGGLE
                                  : held_ms >= 1000 ? SC_IN_HANGUP_REQUEST
                                  : SC_IN_WAKE;
            ESP_LOGI(TAG, "button %d ms -> %s", held_ms,
                     t == SC_IN_WAKE ? "wake" : t == SC_IN_HANGUP_REQUEST ? "hang up" : "mute toggle");
            post_input(t, 0);
            held_ms = 0;
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---------------------------------------------------------------------------

void gls_controller_wifi(bool up)
{
    if (s_started) {
        post_input(up ? SC_IN_WIFI_UP : SC_IN_WIFI_DOWN, 0);
    }
}

esp_err_t gls_controller_set_credentials(const char *chunk0, const char *chunk1)
{
    if (!s_started || !chunk0) {
        return ESP_ERR_INVALID_STATE;
    }
    job_t j = {.type = JOB_SET_CREDS, .chunk0 = strdup(chunk0), .chunk1 = chunk1 ? strdup(chunk1) : NULL,
               .reply = xQueueCreate(1, sizeof(esp_err_t))};
    if (!j.chunk0 || (chunk1 && !j.chunk1) || !j.reply ||
        xQueueSend(s_jobs, &j, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(j.chunk0); free(j.chunk1);
        if (j.reply) vQueueDelete(j.reply);
        return ESP_ERR_NO_MEM;
    }
    // Worker owns auth and cookie storage; don't acknowledge until persistence succeeds.
    esp_err_t result;
    xQueueReceive(j.reply, &result, portMAX_DELAY);
    vQueueDelete(j.reply);
    return result;
}

void gls_controller_get_status(gls_controller_status_t *out)
{
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    out->barge_ins = atomic_load(&s_barge_ins);
    portEXIT_CRITICAL(&s_status_lock);
}
void gls_controller_microphone(const int16_t *pcm, size_t count)
{
    static sc_barge_in_t detector;
    if (sc_barge_in_feed(&detector, pcm, count, gls_now_ms(),
                         atomic_load(&s_assistant_speaking), xvf_i2s_mic_muted())) {
        // Best effort, non-blocking: never stall the high-priority I2S reader.
        ctrl_msg_t msg = {.type = MSG_INPUT, .in = {.type = SC_IN_BARGE_IN}};
        if (s_q && xQueueSend(s_q, &msg, 0) == pdTRUE) s_barge_ins++;
    }
}

void gls_controller_wake(void) { if (s_started) post_input(SC_IN_WAKE, 0); }
void gls_controller_hangup(void) { if (s_started) post_input(SC_IN_HANGUP_REQUEST, 0); }
void gls_controller_mute_toggle(void) { if (s_started) post_input(SC_IN_MUTE_TOGGLE, 0); }

static atomic_int s_volume = CONFIG_GLS_DEFAULT_VOLUME;
static atomic_int s_voice_boost = 6; // dB, ChatGPT voice only

static void persist_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("gls_settings", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, key, v);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "could not persist %s", key);
}

int gls_controller_voice_boost(void) { return atomic_load(&s_voice_boost); }

static xvf_eq_t s_eq;
static portMUX_TYPE s_eq_lock = portMUX_INITIALIZER_UNLOCKED;

void gls_controller_eq(xvf_eq_t *out)
{
    portENTER_CRITICAL(&s_eq_lock);
    *out = s_eq;
    portEXIT_CRITICAL(&s_eq_lock);
}

esp_err_t gls_controller_set_eq(const xvf_eq_t *eq)
{
    if (!xvf_eq_valid(eq)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_eq_lock);
    s_eq = *eq;
    portEXIT_CRITICAL(&s_eq_lock);
    xvf_i2s_set_eq(eq);
    nvs_handle_t h;
    esp_err_t err = nvs_open("gls_settings", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "eq", eq, sizeof(*eq));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "could not persist EQ");
    return ESP_OK;
}

esp_err_t gls_controller_set_voice_boost(int db)
{
    if (db < 0 || db > XVF_VOICE_BOOST_MAX_DB) return ESP_ERR_INVALID_ARG;
    atomic_store(&s_voice_boost, db);
    xvf_i2s_set_voice_boost(db);
    persist_u8("vboost", (uint8_t)db);
    return ESP_OK;
}

int gls_controller_volume(void) { return atomic_load(&s_volume); }

esp_err_t gls_controller_set_volume(int percent)
{
    if (percent < 0 || percent > XVF_VOLUME_MAX) return ESP_ERR_INVALID_ARG;
    atomic_store(&s_volume, percent);
    xvf_i2s_set_volume(percent);
    persist_u8("volume", (uint8_t)percent);
    return ESP_OK;
}
void gls_controller_logout(void) { if (s_started) post_input(SC_IN_LOGOUT, 0); }

esp_err_t gls_controller_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_q = xQueueCreate(32, sizeof(ctrl_msg_t));
    s_jobs = xQueueCreate(8, sizeof(job_t));
    s_offer_q = xQueueCreate(2, sizeof(char *));
    if (!s_q || !s_jobs || !s_offer_q) {
        return ESP_ERR_NO_MEM;
    }

    bool has_creds = chatgpt_session_init();
    update_auth_status(NULL);
    sc_fsm_config_t cfg;
    sc_fsm_config_defaults(&cfg);
    cfg.random_u32 = gls_sc_random;
    cfg.idle_close_s = CONFIG_GLS_IDLE_CLOSE_S;
    sc_fsm_init(&c.fsm, &cfg, has_creds, gls_now_ms());
    // Fresh setup starts muted; subsequent boots remember the user's choice.
    uint8_t muted = 1;
    nvs_handle_t settings;
    if (nvs_open("gls_settings", NVS_READWRITE, &settings) == ESP_OK) {
        if (nvs_get_u8(settings, "muted", &muted) == ESP_ERR_NVS_NOT_FOUND) {
            if (nvs_set_u8(settings, "muted", muted) == ESP_OK) nvs_commit(settings);
        }
        uint8_t vol;
        if (nvs_get_u8(settings, "volume", &vol) == ESP_OK && vol <= XVF_VOLUME_MAX) {
            atomic_store(&s_volume, vol);
            xvf_i2s_set_volume(vol);
        }
        if (nvs_get_u8(settings, "vboost", &vol) == ESP_OK && vol <= XVF_VOICE_BOOST_MAX_DB)
            atomic_store(&s_voice_boost, vol);
        xvf_i2s_set_voice_boost(atomic_load(&s_voice_boost));
        xvf_eq_t eq;
        size_t len = sizeof(eq);
        if (nvs_get_blob(settings, "eq", &eq, &len) == ESP_OK && len == sizeof(eq) && xvf_eq_valid(&eq)) s_eq = eq;
        else s_eq = XVF_EQ_DEFAULT;
        xvf_i2s_set_eq(&s_eq);
        nvs_close(settings);
    }
    c.fsm.muted = muted != 0;
    xvf_i2s_set_mic_mute(c.fsm.muted);
    sc_wm_tracker_init(&c.tracker);
    c.command_index = -1;
    c.last_state = c.fsm.state;
    c.last_led = SC_LED_OFF;
    c.last_explicit_state = SC_RT_UNKNOWN;

    gls_led_init();
    // Worker does TLS + cJSON + esp_peer setup: generous stack, internal RAM (it writes NVS).
    if (xTaskCreatePinnedToCore(worker_task, "gls_worker", 12 * 1024, NULL, 5, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(controller_task, "gls_ctrl", 6 * 1024, NULL, 6, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(button_task, "gls_btn", 3 * 1024, NULL, 4, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "controller up (credentials: %s)", has_creds ? "yes" : "no");

    if (gls_wifi_is_up()) {
        post_input(SC_IN_WIFI_UP, 0);
    }
#if CONFIG_GLS_AUTO_CALL_ON_BOOT
    post_input(SC_IN_WAKE, 0); // FSM keeps the call pending through auth
#endif
    return ESP_OK;
}
