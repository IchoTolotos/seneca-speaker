#include "session_fsm.h"

#include <string.h>

#define S_TO_MS(s) ((uint64_t)(s) * 1000u)

void sc_fsm_config_defaults(sc_fsm_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->idle_close_s = 300;
    c->max_call_s = 7200;
    c->rotate_before_cap_s = 60;
    c->rotate_hard_floor_s = 15;
    c->daily_refresh_s = 86400;
    c->token_refresh_margin_s = 86400;
    c->min_refresh_interval_s = 3600;
    c->token_min_valid_for_call_s = 300;
    c->auth_timeout_s = 30;
    c->signal_timeout_s = 30;
    c->connect_timeout_s = 20;
    c->backoff_base_ms = 1000;
    c->edge_backoff_base_ms = 15000;
    c->backoff_max_ms = 300000;
    c->max_call_attempts = 6;
    c->error_led_after = 3;
}

const char *sc_fsm_state_name(sc_fsm_state_t s)
{
    static const char *n[] = {"wait_wifi", "needs_login", "authing", "idle",
                              "signaling", "connecting", "in_call", "backoff"};
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}

const char *sc_led_name(sc_led_t l)
{
    static const char *n[] = {"off", "idle", "listening", "thinking", "speaking",
                              "connecting", "error", "needs_login", "muted"};
    return (unsigned)l < sizeof n / sizeof n[0] ? n[l] : "?";
}

uint32_t sc_fsm_backoff_ms(const sc_fsm_config_t *cfg, uint32_t failures, sc_fail_kind_t kind)
{
    uint64_t base = kind == SC_FAIL_EDGE ? cfg->edge_backoff_base_ms : cfg->backoff_base_ms;
    if (base == 0) base = 1;
    if (failures == 0) failures = 1;
    uint64_t d = base;
    for (uint32_t i = 1; i < failures && d < cfg->backoff_max_ms; i++) d *= 2;
    if (d > cfg->backoff_max_ms) d = cfg->backoff_max_ms;
    /* "equal jitter": uniformly in [d/2, d] so retries spread but never collapse to 0. */
    uint64_t half = d / 2;
    uint32_t r = cfg->random_u32 ? cfg->random_u32(cfg->random_ctx) : 0;
    return (uint32_t)(half + (r % (d - half + 1)));
}

void sc_fsm_init(sc_fsm_t *f, const sc_fsm_config_t *cfg, bool has_credentials, uint64_t now_ms)
{
    memset(f, 0, sizeof(*f));
    if (cfg)
        f->cfg = *cfg;
    else
        sc_fsm_config_defaults(&f->cfg);
    f->state = SC_ST_WAIT_WIFI;
    f->has_credentials = has_credentials;
    f->last_activity_ms = now_ms;
    f->led = has_credentials ? SC_LED_CONNECTING : SC_LED_NEEDS_LOGIN;
}

/* ---------- helpers ---------- */

static bool remote_active(const sc_fsm_t *f)
{
    return f->remote_state == SC_RT_SPEAKING || f->remote_state == SC_RT_THINKING ||
           f->remote_state == SC_RT_LISTENING_INTENTLY;
}

static bool refresh_due(const sc_fsm_t *f, uint64_t now)
{
    if (!f->have_token || f->token_exp_ms <= now) return true;
    if (now - f->last_auth_ok_ms < S_TO_MS(f->cfg.min_refresh_interval_s)) return false;
    if (f->token_exp_ms < now + S_TO_MS(f->cfg.token_refresh_margin_s)) return true;
    if (f->cfg.daily_refresh_s && now - f->last_auth_ok_ms >= S_TO_MS(f->cfg.daily_refresh_s)) return true;
    return false;
}

static bool token_ok_for_call(const sc_fsm_t *f, uint64_t now)
{
    return f->have_token && f->token_exp_ms > now + S_TO_MS(f->cfg.token_min_valid_for_call_s);
}

static void reset_call(sc_fsm_t *f)
{
    f->research_active = false;
    f->remote_state = SC_RT_UNKNOWN;
    f->rotate_due = false;
}

static void hang_up(sc_fsm_t *f, sc_hangup_reason_t reason, sc_fsm_output_t *out)
{
    out->actions |= SC_ACT_HANG_UP;
    out->hangup_reason = reason;
    reset_call(f);
}

static void to_idle(sc_fsm_t *f)
{
    f->state = SC_ST_IDLE;
    f->call_pending = false;
    f->rotation = false;
}

static void needs_login(sc_fsm_t *f, sc_fsm_output_t *out)
{
    f->state = SC_ST_NEEDS_LOGIN;
    f->has_credentials = false;
    f->have_token = false;
    f->call_pending = false;
    f->rotation = false;
    out->actions |= SC_ACT_SERVE_SETUP;
}

static void start_auth(sc_fsm_t *f, uint64_t now, sc_auth_purpose_t purpose, bool force, sc_fsm_output_t *out)
{
    out->actions |= SC_ACT_START_AUTH;
    out->seq = ++f->seq;
    out->auth_force = force || f->auth_force_next;
    f->auth_force_next = false;
    f->auth_purpose = purpose;
    f->state = SC_ST_AUTHING;
    f->deadline_ms = now + S_TO_MS(f->cfg.auth_timeout_s);
}

static void start_call(sc_fsm_t *f, uint64_t now, sc_fsm_output_t *out)
{
    f->call_pending = true;
    if (!f->has_credentials) {
        needs_login(f, out);
        return;
    }
    if (!f->wifi_up) {
        f->state = SC_ST_WAIT_WIFI;
        return;
    }
    /* Rotations are natural breaks: also roll the cookie there if a refresh is due. */
    if (!token_ok_for_call(f, now) || (f->rotation && refresh_due(f, now))) {
        start_auth(f, now, SC_AUTH_PURPOSE_CALL, false, out);
        return;
    }
    out->actions |= SC_ACT_START_CALL;
    out->seq = ++f->seq;
    out->call_is_rotation = f->rotation;
    f->state = SC_ST_SIGNALING;
    f->deadline_ms = now + S_TO_MS(f->cfg.signal_timeout_s);
}

static void go_backoff(sc_fsm_t *f, uint64_t now, bool is_call, sc_fail_kind_t kind, sc_fsm_output_t *out)
{
    f->failures++;
    if (is_call) {
        f->call_attempts++;
        if (f->cfg.max_call_attempts && f->call_attempts >= f->cfg.max_call_attempts) {
            /* Give up on this call; background refresh keeps retrying from IDLE. */
            f->call_attempts = 0;
            f->error_latched = true;
            to_idle(f);
            if (f->has_credentials && refresh_due(f, now)) {
                is_call = false; /* fall through to an auth backoff */
            } else {
                return;
            }
        }
    }
    uint32_t d = sc_fsm_backoff_ms(&f->cfg, f->failures, kind);
    f->state = SC_ST_BACKOFF;
    f->backoff_is_call = is_call;
    f->deadline_ms = now + d;
    out->actions |= SC_ACT_SCHEDULE_RETRY;
    out->retry_delay_ms = d;
}

static void do_rotate(sc_fsm_t *f, uint64_t now, sc_fsm_output_t *out)
{
    hang_up(f, SC_HANGUP_ROTATE, out);
    f->rotation = true;
    start_call(f, now, out);
}

static bool quiet(const sc_fsm_t *f)
{
    return !remote_active(f) && !f->research_active;
}

static void check_rotation(sc_fsm_t *f, uint64_t now, sc_fsm_output_t *out)
{
    if (f->state != SC_ST_IN_CALL) return;
    if (now >= f->rotate_at_ms) f->rotate_due = true;
    if (f->rotate_due && (quiet(f) || now >= f->rotate_hard_ms)) do_rotate(f, now, out);
}

static void after_wifi_or_creds(sc_fsm_t *f, uint64_t now, sc_fsm_output_t *out)
{
    if (!f->has_credentials) {
        needs_login(f, out);
    } else if (f->call_pending) {
        start_call(f, now, out);
    } else if (refresh_due(f, now)) {
        start_auth(f, now, f->ever_authed ? SC_AUTH_PURPOSE_REFRESH : SC_AUTH_PURPOSE_BOOT, false, out);
    } else {
        to_idle(f);
    }
}

static void user_call_request(sc_fsm_t *f, uint64_t now, sc_fsm_output_t *out)
{
    f->error_latched = false;
    f->call_attempts = 0;
    f->signal_401_retried = false;
    f->rotation = false;
    start_call(f, now, out);
}

static bool seq_ok(const sc_fsm_t *f, const sc_fsm_input_t *in)
{
    return in->seq == 0 || in->seq == f->seq;
}

static sc_led_t compute_led(const sc_fsm_t *f)
{
    switch (f->state) {
    case SC_ST_NEEDS_LOGIN:
        return SC_LED_NEEDS_LOGIN;
    case SC_ST_WAIT_WIFI:
        return f->has_credentials ? SC_LED_CONNECTING : SC_LED_NEEDS_LOGIN;
    case SC_ST_AUTHING:
        if (f->auth_purpose == SC_AUTH_PURPOSE_REFRESH && !f->call_pending)
            return f->muted ? SC_LED_MUTED : (f->error_latched ? SC_LED_ERROR : SC_LED_IDLE);
        return SC_LED_CONNECTING;
    case SC_ST_IDLE:
        if (f->error_latched) return SC_LED_ERROR;
        return f->muted ? SC_LED_MUTED : SC_LED_IDLE;
    case SC_ST_SIGNALING:
    case SC_ST_CONNECTING:
        return SC_LED_CONNECTING;
    case SC_ST_IN_CALL:
        if (f->muted) return SC_LED_MUTED;
        if (f->remote_state == SC_RT_SPEAKING) return SC_LED_SPEAKING;
        if (f->remote_state == SC_RT_THINKING || f->research_active) return SC_LED_THINKING;
        return SC_LED_LISTENING;
    case SC_ST_BACKOFF:
        if (f->failures >= f->cfg.error_led_after || f->error_latched) return SC_LED_ERROR;
        if (f->backoff_is_call || !f->ever_authed) return SC_LED_CONNECTING;
        return f->muted ? SC_LED_MUTED : SC_LED_IDLE;
    }
    return SC_LED_OFF;
}

/* ---------- per-input handlers ---------- */

static void on_tick(sc_fsm_t *f, uint64_t now, sc_fsm_output_t *out)
{
    switch (f->state) {
    case SC_ST_AUTHING:
        if (now >= f->deadline_ms) go_backoff(f, now, f->call_pending, SC_FAIL_NETWORK, out);
        break;
    case SC_ST_SIGNALING:
    case SC_ST_CONNECTING:
        if (now >= f->deadline_ms) {
            hang_up(f, SC_HANGUP_ERROR, out);
            go_backoff(f, now, true, SC_FAIL_NETWORK, out);
        }
        break;
    case SC_ST_BACKOFF:
        if (now >= f->deadline_ms) {
            if (f->backoff_is_call)
                start_call(f, now, out);
            else
                start_auth(f, now, f->ever_authed ? SC_AUTH_PURPOSE_REFRESH : SC_AUTH_PURPOSE_BOOT, false, out);
        }
        break;
    case SC_ST_IDLE:
        if (f->has_credentials && f->wifi_up && refresh_due(f, now))
            start_auth(f, now, f->ever_authed ? SC_AUTH_PURPOSE_REFRESH : SC_AUTH_PURPOSE_BOOT, false, out);
        break;
    case SC_ST_IN_CALL:
        if (remote_active(f) || f->research_active) f->last_activity_ms = now;
        if (f->cfg.idle_close_s && now - f->last_activity_ms >= S_TO_MS(f->cfg.idle_close_s)) {
            hang_up(f, SC_HANGUP_IDLE, out);
            to_idle(f);
            /* Roll the cookie right away if it is due, rather than on the next tick. */
            if (refresh_due(f, now)) start_auth(f, now, SC_AUTH_PURPOSE_REFRESH, false, out);
            break;
        }
        check_rotation(f, now, out);
        break;
    default:
        break;
    }
}

static void on_peer_down(sc_fsm_t *f, const sc_fsm_input_t *in, sc_fsm_output_t *out)
{
    uint64_t now = in->now_ms;
    if (f->state == SC_ST_CONNECTING) {
        hang_up(f, SC_HANGUP_ERROR, out);
        go_backoff(f, now, true, SC_FAIL_NETWORK, out);
        return;
    }
    if (f->state != SC_ST_IN_CALL) return;
    bool due = f->rotate_due || now >= f->rotate_at_ms;
    bool recent = !f->cfg.idle_close_s || now - f->last_activity_ms < S_TO_MS(f->cfg.idle_close_s);
    hang_up(f, SC_HANGUP_ERROR, out);
    if (due) {
        f->rotation = true;
        start_call(f, now, out);
    } else if (recent) {
        /* Unexpected drop mid-conversation: reconnect (same conversation) with backoff. */
        f->rotation = true;
        f->call_pending = true;
        go_backoff(f, now, true, SC_FAIL_NETWORK, out);
    } else {
        to_idle(f);
    }
}

void sc_fsm_handle(sc_fsm_t *f, const sc_fsm_input_t *in, sc_fsm_output_t *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t now = in->now_ms;

    switch (in->type) {
    case SC_IN_TICK:
        on_tick(f, now, out);
        break;

    case SC_IN_WAKE:
        switch (f->state) {
        case SC_ST_NEEDS_LOGIN:
            out->actions |= SC_ACT_SERVE_SETUP;
            break;
        case SC_ST_IDLE:
        case SC_ST_BACKOFF:
            user_call_request(f, now, out);
            break;
        case SC_ST_AUTHING:
            f->call_pending = true;
            f->error_latched = false;
            break;
        case SC_ST_IN_CALL:
            f->last_activity_ms = now;
            if (f->remote_state == SC_RT_SPEAKING) out->actions |= SC_ACT_STOP_SPEAKING;
            break;
        default:
            break;
        }
        break;

    case SC_IN_BARGE_IN:
        if (f->state == SC_ST_IN_CALL && !f->muted && f->remote_state == SC_RT_SPEAKING) {
            f->last_activity_ms = now;
            out->actions |= SC_ACT_STOP_SPEAKING;
        }
        break;

    case SC_IN_HANGUP_REQUEST:
        switch (f->state) {
        case SC_ST_SIGNALING:
        case SC_ST_CONNECTING:
        case SC_ST_IN_CALL:
            hang_up(f, SC_HANGUP_USER, out);
            to_idle(f);
            break;
        case SC_ST_BACKOFF:
            if (f->backoff_is_call) to_idle(f);
            f->call_pending = false;
            f->rotation = false;
            break;
        case SC_ST_AUTHING:
        case SC_ST_WAIT_WIFI:
            f->call_pending = false;
            f->rotation = false;
            break;
        default:
            break;
        }
        break;

    case SC_IN_MUTE_TOGGLE:
        f->muted = !f->muted;
        out->actions |= f->muted ? SC_ACT_SET_MIC_MUTE : SC_ACT_SET_MIC_UNMUTE;
        break;

    case SC_IN_ACTIVITY:
        if (f->state == SC_ST_IN_CALL) f->last_activity_ms = now;
        break;

    case SC_IN_WIFI_UP:
        f->wifi_up = true;
        if (f->state == SC_ST_WAIT_WIFI) after_wifi_or_creds(f, now, out);
        break;

    case SC_IN_WIFI_DOWN: {
        f->wifi_up = false;
        bool was_call = f->state == SC_ST_IN_CALL;
        bool recent = !f->cfg.idle_close_s || now - f->last_activity_ms < S_TO_MS(f->cfg.idle_close_s);
        if (f->state == SC_ST_SIGNALING || f->state == SC_ST_CONNECTING || was_call) {
            hang_up(f, SC_HANGUP_WIFI, out);
            f->call_pending = !was_call || recent;
            f->rotation = was_call && recent;
        }
        if (f->state != SC_ST_NEEDS_LOGIN) f->state = SC_ST_WAIT_WIFI;
        break;
    }

    case SC_IN_CREDENTIALS_SET:
        f->has_credentials = true;
        f->have_token = false;
        f->error_latched = false;
        f->failures = 0;
        switch (f->state) {
        case SC_ST_NEEDS_LOGIN:
        case SC_ST_WAIT_WIFI:
            if (!f->wifi_up) {
                f->state = SC_ST_WAIT_WIFI;
                break;
            }
            /* fallthrough */
        case SC_ST_IDLE:
        case SC_ST_BACKOFF:
        case SC_ST_AUTHING:
            start_auth(f, now, SC_AUTH_PURPOSE_BOOT, true, out);
            break;
        default:
            f->auth_force_next = true; /* in a call: re-auth with the new cookie afterwards */
            break;
        }
        break;

    case SC_IN_LOGOUT:
        if (f->state == SC_ST_SIGNALING || f->state == SC_ST_CONNECTING || f->state == SC_ST_IN_CALL)
            hang_up(f, SC_HANGUP_LOGOUT, out);
        out->actions |= SC_ACT_CLEAR_CREDENTIALS;
        f->ever_authed = false;
        needs_login(f, out);
        break;

    case SC_IN_AUTH_OK:
        if (f->state != SC_ST_AUTHING || !seq_ok(f, in)) break;
        if (in->token_exp_unix != 0 && in->token_exp_unix <= in->now_unix) {
            /* Already expired by our clock: wrong now_unix, or a bad token. Don't spin. */
            go_backoff(f, now, f->call_pending, SC_FAIL_OTHER, out);
            break;
        }
        f->have_token = true;
        if (in->token_exp_unix == 0) /* exp unknown: assume valid, re-check after min_refresh_interval */
            f->token_exp_ms = now + S_TO_MS(f->cfg.token_refresh_margin_s) + S_TO_MS(f->cfg.min_refresh_interval_s);
        else
            f->token_exp_ms = now + (uint64_t)(in->token_exp_unix - in->now_unix) * 1000u;
        f->last_auth_ok_ms = now;
        f->ever_authed = true;
        f->failures = 0;
        if (f->call_pending) {
            start_call(f, now, out);
        } else {
            to_idle(f);
        }
        break;

    case SC_IN_AUTH_FAIL:
        if (f->state != SC_ST_AUTHING || !seq_ok(f, in)) break;
        if (in->fail == SC_FAIL_UNAUTHORIZED) {
            needs_login(f, out);
        } else {
            go_backoff(f, now, f->call_pending, in->fail, out);
        }
        break;

    case SC_IN_SIGNAL_OK:
        if (f->state != SC_ST_SIGNALING || !seq_ok(f, in)) break;
        f->state = SC_ST_CONNECTING;
        f->deadline_ms = now + S_TO_MS(f->cfg.connect_timeout_s);
        break;

    case SC_IN_SIGNAL_FAIL:
        if (f->state != SC_ST_SIGNALING || !seq_ok(f, in)) break;
        hang_up(f, SC_HANGUP_ERROR, out);
        if (in->fail == SC_FAIL_UNAUTHORIZED && !f->signal_401_retried) {
            /* Cached token went stale: mint a fresh one once, then retry. */
            f->signal_401_retried = true;
            f->have_token = false;
            start_auth(f, now, SC_AUTH_PURPOSE_CALL, true, out);
        } else {
            go_backoff(f, now, true, in->fail, out);
        }
        break;

    case SC_IN_PEER_CONNECTED:
        if (f->state != SC_ST_CONNECTING || !seq_ok(f, in)) break;
        f->state = SC_ST_IN_CALL;
        f->call_start_ms = now;
        f->last_activity_ms = now;
        f->failures = 0;
        f->call_attempts = 0;
        f->signal_401_retried = false;
        f->error_latched = false;
        f->call_pending = false;
        f->rotation = false;
        reset_call(f);
        {
            uint32_t cap = f->cfg.max_call_s;
            uint32_t soft = cap > f->cfg.rotate_before_cap_s ? cap - f->cfg.rotate_before_cap_s : 0;
            uint32_t hard = cap > f->cfg.rotate_hard_floor_s ? cap - f->cfg.rotate_hard_floor_s : 0;
            f->rotate_at_ms = now + S_TO_MS(soft);
            f->rotate_hard_ms = now + S_TO_MS(hard);
        }
        if (f->muted) out->actions |= SC_ACT_SET_MIC_MUTE;
        break;

    case SC_IN_PEER_FAILED:
    case SC_IN_PEER_CLOSED:
        if (!seq_ok(f, in)) break;
        on_peer_down(f, in, out);
        break;

    case SC_IN_REMOTE_STATE:
        if (f->state != SC_ST_IN_CALL) break;
        f->remote_state = in->remote_state;
        if (remote_active(f)) f->last_activity_ms = now;
        check_rotation(f, now, out);
        break;

    case SC_IN_REMOTE_CLOSE:
        if (!seq_ok(f, in)) break;
        if (f->state != SC_ST_IN_CALL && f->state != SC_ST_CONNECTING && f->state != SC_ST_SIGNALING) break;
        // An explicit remote close ends the call. Scheduled cap rotation is handled
        // by TICK/USAGE; never redial after the server has deliberately closed it.
        hang_up(f, SC_HANGUP_REMOTE, out);
        to_idle(f);
        break;

    case SC_IN_USAGE:
        if (f->state != SC_ST_IN_CALL) break;
        if (in->hang_up) {
            hang_up(f, SC_HANGUP_REMOTE, out);
            to_idle(f);
            break;
        }
        if (in->has_session_remaining) {
            double r = in->session_remaining_s;
            double soft = r - f->cfg.rotate_before_cap_s, hard = r - f->cfg.rotate_hard_floor_s;
            uint64_t soft_at = now + (soft > 0 ? (uint64_t)(soft * 1000.0) : 0);
            uint64_t hard_at = now + (hard > 0 ? (uint64_t)(hard * 1000.0) : 0);
            if (soft_at < f->rotate_at_ms) f->rotate_at_ms = soft_at;
            if (hard_at < f->rotate_hard_ms) f->rotate_hard_ms = hard_at;
        }
        if (in->reconnect) {
            f->rotate_at_ms = now;
            uint64_t hard_at = now + S_TO_MS(f->cfg.rotate_hard_floor_s);
            if (hard_at < f->rotate_hard_ms) f->rotate_hard_ms = hard_at;
        }
        check_rotation(f, now, out);
        break;

    case SC_IN_SPAWN:
        if (f->state != SC_ST_IN_CALL) break;
        f->research_active = in->spawn_start;
        f->last_activity_ms = now;
        check_rotation(f, now, out);
        break;
    }

    f->led = compute_led(f);
    out->led = f->led;
    out->state = f->state;
}

uint64_t sc_fsm_next_deadline(const sc_fsm_t *f)
{
    switch (f->state) {
    case SC_ST_AUTHING:
    case SC_ST_SIGNALING:
    case SC_ST_CONNECTING:
    case SC_ST_BACKOFF:
        return f->deadline_ms;
    case SC_ST_IDLE: {
        if (!f->has_credentials || !f->wifi_up) return UINT64_MAX;
        if (!f->have_token) return 0;
        uint64_t a = f->token_exp_ms > S_TO_MS(f->cfg.token_refresh_margin_s)
                         ? f->token_exp_ms - S_TO_MS(f->cfg.token_refresh_margin_s)
                         : 0;
        uint64_t floor_at = f->last_auth_ok_ms + S_TO_MS(f->cfg.min_refresh_interval_s);
        if (a < floor_at) a = floor_at < f->token_exp_ms ? floor_at : f->token_exp_ms;
        uint64_t b = f->cfg.daily_refresh_s ? f->last_auth_ok_ms + S_TO_MS(f->cfg.daily_refresh_s) : UINT64_MAX;
        return a < b ? a : b;
    }
    case SC_ST_IN_CALL: {
        uint64_t d = f->rotate_due ? f->rotate_hard_ms : f->rotate_at_ms;
        if (f->cfg.idle_close_s) {
            uint64_t i = f->last_activity_ms + S_TO_MS(f->cfg.idle_close_s);
            if (i < d) d = i;
        }
        return d;
    }
    default:
        return UINT64_MAX;
    }
}
