/*
 * Device lifecycle state machine: pure, event-driven, no I/O, no clocks.
 *
 * The integrator feeds inputs (sc_fsm_input_t, each carrying now_ms from a
 * monotonic clock) from ONE task and executes the returned actions
 * (sc_fsm_output_t). Feed SC_IN_TICK at least once per second; all timers
 * (idle close, rotation, backoff, refresh, timeouts) fire from ticks.
 *
 * Results of actions are fed back as inputs, e.g. SC_ACT_START_AUTH ->
 * (do GET /api/auth/session) -> SC_IN_AUTH_OK / SC_IN_AUTH_FAIL.
 * Inputs for an attempt carry the `seq` from the output that started it;
 * stale results (seq mismatch) are ignored. seq 0 skips the check.
 */
#ifndef SC_SESSION_FSM_H
#define SC_SESSION_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "sc_platform.h"
#include "wm_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SC_ST_WAIT_WIFI = 0, /* no network yet */
    SC_ST_NEEDS_LOGIN,   /* no/invalid ChatGPT session: serve setup page */
    SC_ST_AUTHING,       /* GET /api/auth/session in flight */
    SC_ST_IDLE,          /* logged in, no call */
    SC_ST_SIGNALING,     /* POST /realtime/wm in flight */
    SC_ST_CONNECTING,    /* SDP answer applied, waiting for ICE/DTLS/data channel */
    SC_ST_IN_CALL,
    SC_ST_BACKOFF,       /* waiting to retry auth or call */
} sc_fsm_state_t;

typedef enum {
    SC_LED_OFF = 0,
    SC_LED_IDLE,
    SC_LED_LISTENING,
    SC_LED_THINKING,
    SC_LED_SPEAKING,
    SC_LED_CONNECTING,
    SC_LED_ERROR,
    SC_LED_NEEDS_LOGIN,
    SC_LED_MUTED,
} sc_led_t;

typedef enum {
    SC_FAIL_NETWORK = 0, /* DNS/TLS/timeout/5xx or anything unclassified */
    SC_FAIL_UNAUTHORIZED,/* 401, or 200 without accessToken/email (session invalid) */
    SC_FAIL_EDGE,        /* 403 / cf-mitigated challenge from chatgpt.com's edge */
    SC_FAIL_OTHER,       /* other 4xx */
} sc_fail_kind_t;

typedef enum {
    SC_IN_TICK = 0,
    SC_IN_BARGE_IN,        /* local speech during assistant playback; never initiates a call */
    SC_IN_WAKE,            /* wake word or button press */
    SC_IN_HANGUP_REQUEST,  /* user wants to end the call (long press) */
    SC_IN_MUTE_TOGGLE,
    SC_IN_ACTIVITY,        /* local VAD / any user speech evidence */
    SC_IN_WIFI_UP,
    SC_IN_WIFI_DOWN,
    SC_IN_CREDENTIALS_SET, /* setup page stored new cookies */
    SC_IN_LOGOUT,
    SC_IN_AUTH_OK,         /* token_exp_unix + now_unix */
    SC_IN_AUTH_FAIL,       /* fail */
    SC_IN_SIGNAL_OK,       /* SDP answer received and applied */
    SC_IN_SIGNAL_FAIL,     /* fail */
    SC_IN_PEER_CONNECTED,  /* ICE+DTLS up and data channel open */
    SC_IN_PEER_FAILED,
    SC_IN_PEER_CLOSED,
    SC_IN_REMOTE_STATE,    /* state_update: remote_state */
    SC_IN_REMOTE_CLOSE,    /* goodbye / close_ready / close_request */
    SC_IN_USAGE,           /* usage_update: session_remaining_s, hang_up, reconnect */
    SC_IN_SPAWN,           /* spawn_update: spawn_start / !spawn_start */
} sc_fsm_input_type_t;

typedef struct {
    sc_fsm_input_type_t type;
    uint64_t now_ms;
    uint32_t seq;
    sc_fail_kind_t fail;
    int64_t token_exp_unix; /* AUTH_OK: JWT exp (sc_jwt_get_exp); 0 if unknown */
    int64_t now_unix;       /* AUTH_OK: current unix time; use the response's HTTP Date header
                               if SNTP is not synced yet. Only exp - now_unix matters. */
    sc_rt_state_t remote_state;
    bool has_session_remaining;
    double session_remaining_s;
    bool hang_up;
    bool reconnect;
    bool spawn_start;
} sc_fsm_input_t;

/* Action bits */
#define SC_ACT_START_AUTH (1u << 0)     /* GET /api/auth/session; persist rotated cookies */
#define SC_ACT_START_CALL (1u << 1)     /* create peer, offer, POST /realtime/wm with seq */
#define SC_ACT_HANG_UP (1u << 2)        /* tear down peer connection (if any) */
#define SC_ACT_STOP_SPEAKING (1u << 3)  /* send action_request stop_speaking */
#define SC_ACT_SERVE_SETUP (1u << 4)    /* make the setup page reachable (start AP if needed) */
#define SC_ACT_SET_MIC_MUTE (1u << 5)
#define SC_ACT_SET_MIC_UNMUTE (1u << 6)
#define SC_ACT_SCHEDULE_RETRY (1u << 7) /* informational: retry in retry_delay_ms (FSM fires it on TICK) */
#define SC_ACT_CLEAR_CREDENTIALS (1u << 8) /* on logout: erase cookie store */

typedef enum {
    SC_HANGUP_NONE = 0,
    SC_HANGUP_USER,
    SC_HANGUP_IDLE,
    SC_HANGUP_REMOTE,
    SC_HANGUP_ROTATE,
    SC_HANGUP_ERROR,
    SC_HANGUP_WIFI,
    SC_HANGUP_LOGOUT,
} sc_hangup_reason_t;

typedef struct {
    uint32_t actions;
    uint32_t seq;                 /* attempt id for START_AUTH / START_CALL */
    bool auth_force;              /* START_AUTH: ignore any cached token */
    bool call_is_rotation;        /* START_CALL: continue conversation (reuse conversation_id if known) */
    sc_hangup_reason_t hangup_reason;
    uint32_t retry_delay_ms;
    sc_led_t led;
    sc_fsm_state_t state;
} sc_fsm_output_t;

typedef struct {
    uint32_t idle_close_s;              /* close call after no speech; 0 = never. default 300 */
    uint32_t max_call_s;                /* assumed hard session cap. default 7200 */
    uint32_t rotate_before_cap_s;       /* rotate when remaining < this. default 60 */
    uint32_t rotate_hard_floor_s;       /* rotate even mid-speech below this. default 15 */
    uint32_t daily_refresh_s;           /* refresh token (roll cookie) even when idle. default 86400 */
    uint32_t token_refresh_margin_s;    /* refresh when token expires within this. default 86400 */
    uint32_t min_refresh_interval_s;    /* never refresh more often than this while the token is still valid
                                           (guards against short-lived tokens). default 3600 */
    uint32_t token_min_valid_for_call_s;/* need this much token validity to start a call. default 300 */
    uint32_t auth_timeout_s;            /* default 30 */
    uint32_t signal_timeout_s;          /* default 30 */
    uint32_t connect_timeout_s;         /* default 20 */
    uint32_t backoff_base_ms;           /* default 1000 */
    uint32_t edge_backoff_base_ms;      /* first retry after a 403 edge rejection. default 15000 */
    uint32_t backoff_max_ms;            /* default 300000 */
    uint32_t max_call_attempts;         /* consecutive failed call attempts before giving up. default 6 */
    uint32_t error_led_after;           /* consecutive failures before LED shows error. default 3 */
    sc_random_fn random_u32;            /* jitter source (required) */
    void *random_ctx;
} sc_fsm_config_t;

void sc_fsm_config_defaults(sc_fsm_config_t *cfg);

typedef enum { SC_AUTH_PURPOSE_BOOT = 0, SC_AUTH_PURPOSE_REFRESH, SC_AUTH_PURPOSE_CALL } sc_auth_purpose_t;

typedef struct {
    sc_fsm_config_t cfg;
    sc_fsm_state_t state;
    bool wifi_up;
    bool has_credentials;
    bool muted;
    bool call_pending;     /* user wants a call once auth/backoff is done */
    bool rotation;         /* the pending/next call is a rotation */
    bool error_latched;    /* last call attempt gave up; shows ERROR until next wake */
    sc_auth_purpose_t auth_purpose;
    bool auth_force_next;
    bool signal_401_retried;
    uint32_t seq;
    uint32_t failures;     /* consecutive failures (backoff exponent) */
    uint32_t call_attempts;
    uint64_t deadline_ms;  /* timeout for AUTHING/SIGNALING/CONNECTING, retry time for BACKOFF */
    bool backoff_is_call;  /* BACKOFF retries a call (else auth) */
    /* token */
    bool have_token;
    uint64_t token_exp_ms; /* in now_ms timebase */
    uint64_t last_auth_ok_ms;
    bool ever_authed;
    /* call */
    uint64_t call_start_ms;
    uint64_t last_activity_ms;
    uint64_t rotate_at_ms;      /* soft: rotate at the next quiet moment after this */
    uint64_t rotate_hard_ms;    /* hard: rotate regardless */
    bool rotate_due;
    sc_rt_state_t remote_state;
    bool research_active;
    sc_led_t led;
} sc_fsm_t;

/** has_credentials: a session cookie is stored. Starts in WAIT_WIFI. */
void sc_fsm_init(sc_fsm_t *f, const sc_fsm_config_t *cfg, bool has_credentials, uint64_t now_ms);

/** Feeds one input; out is fully overwritten. */
void sc_fsm_handle(sc_fsm_t *f, const sc_fsm_input_t *in, sc_fsm_output_t *out);

/** Earliest now_ms at which a TICK could change something (UINT64_MAX if none). */
uint64_t sc_fsm_next_deadline(const sc_fsm_t *f);

const char *sc_fsm_state_name(sc_fsm_state_t s);
const char *sc_led_name(sc_led_t l);

/** Backoff delay for the given consecutive failure count (1-based); exposed for tests. */
uint32_t sc_fsm_backoff_ms(const sc_fsm_config_t *cfg, uint32_t failures, sc_fail_kind_t kind);

#ifdef __cplusplus
}
#endif
#endif
