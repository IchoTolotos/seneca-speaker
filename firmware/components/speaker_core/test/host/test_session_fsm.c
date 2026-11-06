#include <stdint.h>
#include <stdlib.h>

#include "session_fsm.h"
#include "test.h"

#define SEC 1000ull
#define MIN (60 * SEC)
#define HOUR (60 * MIN)
#define DAY (24 * HOUR)
#define UNIX0 1761300000ll /* wall clock at now_ms == 0 */

typedef struct {
    sc_fsm_t f;
    sc_fsm_output_t o;
    uint32_t acc; /* actions accumulated by tick_until */
    sc_hangup_reason_t last_hangup;
    uint64_t now;
    uint32_t seed;
} rig_t;

static void rig_init(rig_t *r, bool creds, void (*tweak)(sc_fsm_config_t *))
{
    memset(r, 0, sizeof *r);
    r->seed = 1234;
    sc_fsm_config_t cfg;
    sc_fsm_config_defaults(&cfg);
    cfg.random_u32 = (sc_random_fn)sc_test_rand;
    cfg.random_ctx = &r->seed;
    if (tweak) tweak(&cfg);
    sc_fsm_init(&r->f, &cfg, creds, 0);
}

static sc_fsm_input_t in_of(rig_t *r, sc_fsm_input_type_t t)
{
    sc_fsm_input_t in;
    memset(&in, 0, sizeof in);
    in.type = t;
    in.now_ms = r->now;
    return in;
}

static void send_in(rig_t *r, const sc_fsm_input_t *in)
{
    sc_fsm_handle(&r->f, in, &r->o);
    if (r->o.actions & SC_ACT_HANG_UP) r->last_hangup = r->o.hangup_reason;
}

static void feed(rig_t *r, sc_fsm_input_type_t t)
{
    sc_fsm_input_t in = in_of(r, t);
    send_in(r, &in);
}

static void fail(rig_t *r, sc_fsm_input_type_t t, sc_fail_kind_t k)
{
    sc_fsm_input_t in = in_of(r, t);
    in.fail = k;
    send_in(r, &in);
}

static void auth_ok(rig_t *r, int64_t valid_s)
{
    sc_fsm_input_t in = in_of(r, SC_IN_AUTH_OK);
    in.now_unix = UNIX0 + (int64_t)(r->now / 1000);
    in.token_exp_unix = in.now_unix + valid_s;
    send_in(r, &in);
}

static void remote(rig_t *r, sc_rt_state_t s)
{
    sc_fsm_input_t in = in_of(r, SC_IN_REMOTE_STATE);
    in.remote_state = s;
    send_in(r, &in);
}

static void usage(rig_t *r, double remaining, bool hang_up, bool reconnect)
{
    sc_fsm_input_t in = in_of(r, SC_IN_USAGE);
    in.has_session_remaining = remaining >= 0;
    in.session_remaining_s = remaining;
    in.hang_up = hang_up;
    in.reconnect = reconnect;
    send_in(r, &in);
}

/* Ticks once per second until `until` (inclusive) or until an action in `stop_mask` fires. */
static void tick_until(rig_t *r, uint64_t until, uint32_t stop_mask)
{
    r->acc = 0;
    while (r->now < until) {
        r->now += SEC;
        if (r->now > until) r->now = until;
        feed(r, SC_IN_TICK);
        r->acc |= r->o.actions;
        if (r->o.actions & stop_mask) return;
    }
}

static const int64_t TOKEN_VALID = 7 * 86400 + 12 * 3600; /* ~7.5 days */

static void boot_to_idle(rig_t *r)
{
    feed(r, SC_IN_WIFI_UP);
    CHECK(r->o.actions & SC_ACT_START_AUTH);
    CHECK_EQ_INT(r->o.led, SC_LED_CONNECTING);
    auth_ok(r, TOKEN_VALID);
    CHECK_EQ_INT(r->f.state, SC_ST_IDLE);
    CHECK_EQ_INT(r->o.led, SC_LED_IDLE);
}

static void call_up(rig_t *r)
{
    feed(r, SC_IN_WAKE);
    CHECK(r->o.actions & SC_ACT_START_CALL);
    CHECK_EQ_INT(r->o.led, SC_LED_CONNECTING);
    feed(r, SC_IN_SIGNAL_OK);
    CHECK_EQ_INT(r->f.state, SC_ST_CONNECTING);
    feed(r, SC_IN_PEER_CONNECTED);
    CHECK_EQ_INT(r->f.state, SC_ST_IN_CALL);
    CHECK_EQ_INT(r->o.led, SC_LED_LISTENING);
    remote(r, SC_RT_LISTENING);
}

/* ---------------- scenarios ---------------- */

static void first_boot_needs_login_then_credentials(void)
{
    rig_t r;
    rig_init(&r, false, NULL);
    CHECK_EQ_INT(r.f.led, SC_LED_NEEDS_LOGIN);
    feed(&r, SC_IN_WIFI_UP);
    CHECK_EQ_INT(r.f.state, SC_ST_NEEDS_LOGIN);
    CHECK(r.o.actions & SC_ACT_SERVE_SETUP);
    CHECK_EQ_INT(r.o.led, SC_LED_NEEDS_LOGIN);
    feed(&r, SC_IN_WAKE);
    CHECK(!(r.o.actions & SC_ACT_START_CALL));
    feed(&r, SC_IN_CREDENTIALS_SET);
    CHECK(r.o.actions & SC_ACT_START_AUTH);
    CHECK(r.o.auth_force);
    auth_ok(&r, TOKEN_VALID);
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
}

static void auth_401_goes_to_needs_login(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    feed(&r, SC_IN_WIFI_UP);
    fail(&r, SC_IN_AUTH_FAIL, SC_FAIL_UNAUTHORIZED);
    CHECK_EQ_INT(r.f.state, SC_ST_NEEDS_LOGIN);
    CHECK(r.o.actions & SC_ACT_SERVE_SETUP);
    CHECK_EQ_INT(r.o.led, SC_LED_NEEDS_LOGIN);
    /* stays there: no retries, wake only re-announces the setup page */
    tick_until(&r, r.now + HOUR, 0);
    CHECK(!(r.acc & (SC_ACT_START_AUTH | SC_ACT_START_CALL)));
    feed(&r, SC_IN_WAKE);
    CHECK(r.o.actions & SC_ACT_SERVE_SETUP);

    /* 401 while authing for a call also lands in needs_login */
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    r.now += 8 * DAY; /* token expired */
    feed(&r, SC_IN_WAKE);
    CHECK(r.o.actions & SC_ACT_START_AUTH);
    fail(&r, SC_IN_AUTH_FAIL, SC_FAIL_UNAUTHORIZED);
    CHECK_EQ_INT(r.f.state, SC_ST_NEEDS_LOGIN);
}

static void call_led_states_and_barge_in(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    call_up(&r);
    remote(&r, SC_RT_THINKING);
    CHECK_EQ_INT(r.o.led, SC_LED_THINKING);
    remote(&r, SC_RT_SPEAKING);
    CHECK_EQ_INT(r.o.led, SC_LED_SPEAKING);
    feed(&r, SC_IN_WAKE);
    CHECK(r.o.actions & SC_ACT_STOP_SPEAKING);
    remote(&r, SC_RT_LISTENING);
    feed(&r, SC_IN_WAKE);
    CHECK(!(r.o.actions & SC_ACT_STOP_SPEAKING));
    feed(&r, SC_IN_MUTE_TOGGLE);
    CHECK(r.o.actions & SC_ACT_SET_MIC_MUTE);
    CHECK_EQ_INT(r.o.led, SC_LED_MUTED);
    feed(&r, SC_IN_MUTE_TOGGLE);
    CHECK(r.o.actions & SC_ACT_SET_MIC_UNMUTE);
    CHECK_EQ_INT(r.o.led, SC_LED_LISTENING);
    sc_fsm_input_t sp = in_of(&r, SC_IN_SPAWN);
    sp.spawn_start = true;
    send_in(&r, &sp);
    CHECK_EQ_INT(r.o.led, SC_LED_THINKING); /* background research */
    sp.spawn_start = false;
    send_in(&r, &sp);
    CHECK_EQ_INT(r.o.led, SC_LED_LISTENING);
    feed(&r, SC_IN_HANGUP_REQUEST);
    CHECK(r.o.actions & SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.o.hangup_reason, SC_HANGUP_USER);
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
}

static void idle_close_after_five_minutes_of_silence(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    call_up(&r);
    uint64_t t0 = r.now;
    tick_until(&r, t0 + 5 * MIN - SEC, SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.f.state, SC_ST_IN_CALL);
    tick_until(&r, t0 + 5 * MIN, SC_ACT_HANG_UP);
    CHECK(r.acc & SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.last_hangup, SC_HANGUP_IDLE);
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
    CHECK_EQ_INT(r.o.led, SC_LED_IDLE);

    /* speaking / thinking / research keep the call alive */
    call_up(&r);
    t0 = r.now;
    remote(&r, SC_RT_SPEAKING);
    tick_until(&r, t0 + 8 * MIN, SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.f.state, SC_ST_IN_CALL);
    remote(&r, SC_RT_LISTENING);
    sc_fsm_input_t sp = in_of(&r, SC_IN_SPAWN);
    sp.spawn_start = true;
    send_in(&r, &sp);
    tick_until(&r, r.now + 10 * MIN, SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.f.state, SC_ST_IN_CALL);
    sp = in_of(&r, SC_IN_SPAWN);
    send_in(&r, &sp); /* research ended */
    t0 = r.now;
    tick_until(&r, t0 + 4 * MIN, SC_ACT_HANG_UP);
    feed(&r, SC_IN_ACTIVITY); /* local VAD heard the user */
    tick_until(&r, r.now + 4 * MIN, SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.f.state, SC_ST_IN_CALL);
    tick_until(&r, r.now + 2 * MIN, SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);

    /* configurable */
    rig_t q;
    rig_init(&q, true, NULL);
    q.f.cfg.idle_close_s = 60;
    boot_to_idle(&q);
    call_up(&q);
    tick_until(&q, q.now + 61 * SEC, SC_ACT_HANG_UP);
    CHECK_EQ_INT(q.last_hangup, SC_HANGUP_IDLE);
}

static void keep_talking(rig_t *r, uint64_t until, uint32_t stop_mask)
{
    /* user talks every 60 s so idle-close never fires */
    while (r->now < until) {
        feed(r, SC_IN_ACTIVITY);
        uint64_t next = r->now + MIN < until ? r->now + MIN : until;
        tick_until(r, next, stop_mask);
        if (r->acc & stop_mask) return;
    }
}

static void rotates_before_two_hour_cap_without_usage_events(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    call_up(&r);
    uint64_t t0 = r.now;
    keep_talking(&r, t0 + 2 * HOUR, SC_ACT_HANG_UP);
    CHECK(r.acc & SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.last_hangup, SC_HANGUP_ROTATE);
    CHECK(r.o.actions & SC_ACT_START_CALL);
    CHECK(r.o.call_is_rotation);
    CHECK_EQ_INT(r.now - t0, 2 * HOUR - 60 * SEC);
    feed(&r, SC_IN_SIGNAL_OK);
    feed(&r, SC_IN_PEER_CONNECTED);
    CHECK_EQ_INT(r.f.state, SC_ST_IN_CALL);
    /* the new call gets a fresh 2 h budget */
    uint64_t t1 = r.now;
    keep_talking(&r, t1 + HOUR, SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.f.state, SC_ST_IN_CALL);
}

static void usage_update_rotation_waits_for_quiet_moment(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    call_up(&r);
    remote(&r, SC_RT_SPEAKING);
    usage(&r, 7170.2, false, false); /* plenty left */
    CHECK(!(r.o.actions & SC_ACT_HANG_UP));
    r.now += 10 * SEC;
    usage(&r, 45.0, false, false); /* below 60 s, but the assistant is speaking */
    CHECK(!(r.o.actions & SC_ACT_HANG_UP));
    CHECK(r.f.rotate_due);
    r.now += 3 * SEC;
    remote(&r, SC_RT_LISTENING); /* quiet: rotate now */
    CHECK(r.o.actions & SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.o.hangup_reason, SC_HANGUP_ROTATE);
    CHECK(r.o.actions & SC_ACT_START_CALL);
    CHECK(r.o.call_is_rotation);

    /* never quiet: forced at the hard floor (15 s before the cap) */
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    call_up(&r);
    remote(&r, SC_RT_SPEAKING);
    uint64_t t0 = r.now;
    usage(&r, 45.0, false, false);
    tick_until(&r, t0 + 60 * SEC, SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.last_hangup, SC_HANGUP_ROTATE);
    CHECK_EQ_INT(r.now - t0, 30 * SEC);
}

static void usage_instructions_hang_up_and_reconnect(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    call_up(&r);
    usage(&r, 3000, false, true); /* server asks for reconnect; we are listening -> immediate */
    CHECK(r.o.actions & SC_ACT_HANG_UP);
    CHECK(r.o.actions & SC_ACT_START_CALL);
    CHECK(r.o.call_is_rotation);
    feed(&r, SC_IN_SIGNAL_OK);
    feed(&r, SC_IN_PEER_CONNECTED);
    usage(&r, -1, true, false);
    CHECK(r.o.actions & SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.o.hangup_reason, SC_HANGUP_REMOTE);
    CHECK(!(r.o.actions & SC_ACT_START_CALL));
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
    /* goodbye/close_ready ends the call */
    call_up(&r);
    feed(&r, SC_IN_REMOTE_CLOSE);
    CHECK_EQ_INT(r.o.hangup_reason, SC_HANGUP_REMOTE);
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
}

static void remote_close_ends_without_redial_even_at_cap(void)
{
    rig_t r;rig_init(&r,true,NULL);boot_to_idle(&r);call_up(&r);
    r.now=r.f.rotate_at_ms;
    feed(&r,SC_IN_REMOTE_CLOSE);
    CHECK_EQ_INT(r.f.state,SC_ST_IDLE);
    CHECK(r.o.actions&SC_ACT_HANG_UP);
    CHECK(!(r.o.actions&SC_ACT_START_CALL));
    feed(&r,SC_IN_WAKE);
    CHECK_EQ_INT(r.f.state,SC_ST_SIGNALING);
    sc_fsm_input_t stale=in_of(&r,SC_IN_REMOTE_CLOSE);stale.seq=r.f.seq+1;
    send_in(&r,&stale);CHECK_EQ_INT(r.f.state,SC_ST_SIGNALING);
    feed(&r,SC_IN_REMOTE_CLOSE);CHECK_EQ_INT(r.f.state,SC_ST_IDLE);
    feed(&r,SC_IN_WAKE);feed(&r,SC_IN_SIGNAL_OK);
    CHECK_EQ_INT(r.f.state,SC_ST_CONNECTING);
    feed(&r,SC_IN_REMOTE_CLOSE);CHECK_EQ_INT(r.f.state,SC_ST_IDLE);
}

static void backoff_growth_and_cap(void)
{
    sc_fsm_config_t cfg;
    sc_fsm_config_defaults(&cfg);
    uint32_t seed = 99;
    cfg.random_u32 = (sc_random_fn)sc_test_rand;
    cfg.random_ctx = &seed;
    for (uint32_t n = 1; n <= 20; n++) {
        uint64_t d = 1000ull << (n - 1 < 20 ? n - 1 : 20);
        if (d > cfg.backoff_max_ms) d = cfg.backoff_max_ms;
        for (int k = 0; k < 50; k++) {
            uint32_t v = sc_fsm_backoff_ms(&cfg, n, SC_FAIL_NETWORK);
            CHECK(v >= d / 2 && v <= d);
        }
    }
    CHECK(sc_fsm_backoff_ms(&cfg, 1, SC_FAIL_EDGE) >= 7500);
    CHECK(sc_fsm_backoff_ms(&cfg, 30, SC_FAIL_EDGE) <= 300000);
    /* jitter actually varies */
    uint32_t a = sc_fsm_backoff_ms(&cfg, 6, SC_FAIL_NETWORK), b = a;
    for (int k = 0; k < 10 && b == a; k++) b = sc_fsm_backoff_ms(&cfg, 6, SC_FAIL_NETWORK);
    CHECK(a != b);

    /* FSM: repeated 403 edge rejections on boot back off, grow, cap, then recover */
    rig_t r;
    rig_init(&r, true, NULL);
    feed(&r, SC_IN_WIFI_UP);
    uint32_t prev_max = 0;
    for (int i = 1; i <= 10; i++) {
        CHECK_EQ_INT(r.f.state, SC_ST_AUTHING);
        fail(&r, SC_IN_AUTH_FAIL, SC_FAIL_EDGE);
        CHECK_EQ_INT(r.f.state, SC_ST_BACKOFF);
        CHECK(r.o.actions & SC_ACT_SCHEDULE_RETRY);
        uint32_t d = r.o.retry_delay_ms;
        uint64_t cap = 15000ull << (i - 1);
        if (cap > 300000) cap = 300000;
        CHECK(d >= cap / 2 && d <= cap);
        CHECK(d <= 300000);
        if (i <= 5) CHECK(cap > prev_max);
        prev_max = (uint32_t)cap;
        CHECK_EQ_INT(r.o.led, i >= 3 ? SC_LED_ERROR : SC_LED_CONNECTING);
        uint64_t due = r.now + d;
        tick_until(&r, due - 1, SC_ACT_START_AUTH);
        CHECK(!(r.acc & SC_ACT_START_AUTH));
        tick_until(&r, due + SEC, SC_ACT_START_AUTH);
        CHECK(r.acc & SC_ACT_START_AUTH);
    }
    auth_ok(&r, TOKEN_VALID);
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
    CHECK_EQ_INT(r.f.failures, 0);
    CHECK_EQ_INT(r.o.led, SC_LED_IDLE);
}

static void daily_refresh_and_expiry_margin(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    uint64_t t0 = r.now;
    tick_until(&r, t0 + DAY - SEC, SC_ACT_START_AUTH);
    CHECK(!(r.acc & SC_ACT_START_AUTH));
    tick_until(&r, t0 + DAY, SC_ACT_START_AUTH);
    CHECK(r.acc & SC_ACT_START_AUTH);
    CHECK(!r.o.auth_force);
    CHECK_EQ_INT(r.o.led, SC_LED_IDLE); /* background refresh is silent */
    /* a wake during the background refresh turns it into a call */
    feed(&r, SC_IN_WAKE);
    CHECK_EQ_INT(r.o.led, SC_LED_CONNECTING);
    auth_ok(&r, TOKEN_VALID);
    CHECK(r.o.actions & SC_ACT_START_CALL);
    feed(&r, SC_IN_HANGUP_REQUEST);

    /* token with < 24 h left is refreshed, but not more often than hourly */
    rig_init(&r, true, NULL);
    feed(&r, SC_IN_WIFI_UP);
    auth_ok(&r, 20 * 3600);
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
    t0 = r.now;
    tick_until(&r, t0 + HOUR, SC_ACT_START_AUTH);
    CHECK(r.acc & SC_ACT_START_AUTH);
    CHECK_EQ_INT(r.now - t0, HOUR);
    CHECK_EQ_INT(sc_fsm_next_deadline(&r.f), r.f.deadline_ms);

    /* refresh failures while idle retry in the background without error LED at first */
    fail(&r, SC_IN_AUTH_FAIL, SC_FAIL_NETWORK);
    CHECK_EQ_INT(r.f.state, SC_ST_BACKOFF);
    CHECK_EQ_INT(r.o.led, SC_LED_IDLE);
    /* a wake while the old token is still valid calls immediately */
    feed(&r, SC_IN_WAKE);
    CHECK(r.o.actions & SC_ACT_START_CALL);

    /* daily refresh is deferred during a call and happens after it */
    rig_init(&r, true, NULL);
    r.f.cfg.idle_close_s = 0; /* always-on call */
    boot_to_idle(&r);
    call_up(&r);
    t0 = r.now;
    int rotations = 0;
    bool authed = false;
    while (r.now < t0 + DAY + 3 * HOUR) {
        r.now += SEC;
        feed(&r, SC_IN_TICK);
        if (r.o.actions & SC_ACT_START_AUTH) {
            authed = true;
            break;
        }
        CHECK(!(r.o.actions & SC_ACT_HANG_UP) || r.o.hangup_reason == SC_HANGUP_ROTATE);
        if (r.o.actions & SC_ACT_START_CALL) {
            rotations++;
            feed(&r, SC_IN_SIGNAL_OK);
            feed(&r, SC_IN_PEER_CONNECTED);
        }
    }
    /* 12 rotations (every 7140 s) within the first day; the 13th, past 24 h, re-authenticates first */
    CHECK(authed);
    CHECK_EQ_INT(rotations, 12);
    CHECK_EQ_INT(r.now - t0, 13 * 7140 * SEC);
    CHECK(r.f.rotation);
    auth_ok(&r, TOKEN_VALID);
    CHECK(r.o.actions & SC_ACT_START_CALL);
    CHECK(r.o.call_is_rotation);
}

static void signal_401_reauths_once_then_backs_off(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    feed(&r, SC_IN_WAKE);
    fail(&r, SC_IN_SIGNAL_FAIL, SC_FAIL_UNAUTHORIZED);
    CHECK(r.o.actions & SC_ACT_START_AUTH);
    CHECK(r.o.auth_force);
    auth_ok(&r, TOKEN_VALID);
    CHECK(r.o.actions & SC_ACT_START_CALL);
    fail(&r, SC_IN_SIGNAL_FAIL, SC_FAIL_UNAUTHORIZED);
    CHECK_EQ_INT(r.f.state, SC_ST_BACKOFF);
    CHECK(r.o.actions & SC_ACT_SCHEDULE_RETRY);
    tick_until(&r, r.now + 5 * SEC, SC_ACT_START_CALL);
    CHECK(r.acc & SC_ACT_START_CALL);
    /* 403 at signaling: backoff with the edge base */
    fail(&r, SC_IN_SIGNAL_FAIL, SC_FAIL_EDGE);
    CHECK(r.o.retry_delay_ms >= 15000);
}

static void call_attempts_give_up_then_wake_clears_error(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    feed(&r, SC_IN_WAKE);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ_INT(r.f.state, SC_ST_SIGNALING);
        fail(&r, SC_IN_SIGNAL_FAIL, SC_FAIL_NETWORK);
        if (r.f.state == SC_ST_BACKOFF) tick_until(&r, r.f.deadline_ms + SEC, SC_ACT_START_CALL);
    }
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
    CHECK_EQ_INT(r.o.led, SC_LED_ERROR);
    feed(&r, SC_IN_WAKE);
    CHECK(r.o.actions & SC_ACT_START_CALL);
    CHECK_EQ_INT(r.o.led, SC_LED_CONNECTING);
}

static void peer_drop_reconnects_only_if_recent(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    call_up(&r);
    r.now += 30 * SEC;
    feed(&r, SC_IN_PEER_FAILED);
    CHECK(r.o.actions & SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.f.state, SC_ST_BACKOFF);
    tick_until(&r, r.now + 5 * SEC, SC_ACT_START_CALL);
    CHECK(r.acc & SC_ACT_START_CALL);
    CHECK(r.o.call_is_rotation);

    /* connect timeout */
    feed(&r, SC_IN_SIGNAL_OK);
    tick_until(&r, r.now + 25 * SEC, SC_ACT_HANG_UP);
    CHECK(r.acc & SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.f.state, SC_ST_BACKOFF);
    feed(&r, SC_IN_HANGUP_REQUEST);
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
}

static void wifi_loss_resumes_recent_call(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    call_up(&r);
    feed(&r, SC_IN_WIFI_DOWN);
    CHECK(r.o.actions & SC_ACT_HANG_UP);
    CHECK_EQ_INT(r.o.hangup_reason, SC_HANGUP_WIFI);
    CHECK_EQ_INT(r.f.state, SC_ST_WAIT_WIFI);
    CHECK_EQ_INT(r.o.led, SC_LED_CONNECTING);
    r.now += 20 * SEC;
    feed(&r, SC_IN_WIFI_UP);
    CHECK(r.o.actions & SC_ACT_START_CALL);
    CHECK(r.o.call_is_rotation);
}

static void stale_results_are_ignored(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    feed(&r, SC_IN_WIFI_UP);
    uint32_t seq = r.o.seq;
    CHECK(seq != 0);
    sc_fsm_input_t in = in_of(&r, SC_IN_AUTH_FAIL);
    in.fail = SC_FAIL_UNAUTHORIZED;
    in.seq = seq + 7;
    send_in(&r, &in);
    CHECK_EQ_INT(r.f.state, SC_ST_AUTHING);
    /* auth timeout */
    tick_until(&r, r.now + 31 * SEC, SC_ACT_SCHEDULE_RETRY);
    CHECK_EQ_INT(r.f.state, SC_ST_BACKOFF);
    /* late OK for the timed-out attempt is ignored */
    in = in_of(&r, SC_IN_AUTH_OK);
    in.seq = seq;
    in.token_exp_unix = UNIX0 + TOKEN_VALID;
    in.now_unix = UNIX0;
    send_in(&r, &in);
    CHECK_EQ_INT(r.f.state, SC_ST_BACKOFF);
}

static void logout_clears_everything(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    boot_to_idle(&r);
    call_up(&r);
    feed(&r, SC_IN_LOGOUT);
    CHECK(r.o.actions & SC_ACT_HANG_UP);
    CHECK(r.o.actions & SC_ACT_CLEAR_CREDENTIALS);
    CHECK(r.o.actions & SC_ACT_SERVE_SETUP);
    CHECK_EQ_INT(r.f.state, SC_ST_NEEDS_LOGIN);
    CHECK_STR(sc_fsm_state_name(r.f.state), "needs_login");
    CHECK_STR(sc_led_name(r.o.led), "needs_login");
}

static void expired_or_unknown_token_exp(void)
{
    rig_t r;
    rig_init(&r, true, NULL);
    feed(&r, SC_IN_WIFI_UP);
    auth_ok(&r, -10); /* exp in the past by our clock: back off instead of spinning */
    CHECK_EQ_INT(r.f.state, SC_ST_BACKOFF);
    tick_until(&r, r.f.deadline_ms + SEC, SC_ACT_START_AUTH);
    sc_fsm_input_t in = in_of(&r, SC_IN_AUTH_OK); /* exp unknown */
    send_in(&r, &in);
    CHECK_EQ_INT(r.f.state, SC_ST_IDLE);
    uint64_t t0 = r.now;
    tick_until(&r, t0 + 2 * HOUR, SC_ACT_START_AUTH);
    CHECK(r.acc & SC_ACT_START_AUTH);
    CHECK(r.now - t0 >= HOUR && r.now - t0 <= HOUR + SEC);
}

void suite_session_fsm(void)
{
    RUN(expired_or_unknown_token_exp);
    RUN(first_boot_needs_login_then_credentials);
    RUN(auth_401_goes_to_needs_login);
    RUN(call_led_states_and_barge_in);
    RUN(idle_close_after_five_minutes_of_silence);
    RUN(rotates_before_two_hour_cap_without_usage_events);
    RUN(usage_update_rotation_waits_for_quiet_moment);
    RUN(usage_instructions_hang_up_and_reconnect);
    RUN(remote_close_ends_without_redial_even_at_cap);
    RUN(backoff_growth_and_cap);
    RUN(daily_refresh_and_expiry_margin);
    RUN(signal_401_reauths_once_then_backs_off);
    RUN(call_attempts_give_up_then_wake_clears_error);
    RUN(peer_drop_reconnects_only_if_recent);
    RUN(wifi_loss_resumes_recent_call);
    RUN(stale_results_are_ignored);
    RUN(logout_clears_everything);
}
