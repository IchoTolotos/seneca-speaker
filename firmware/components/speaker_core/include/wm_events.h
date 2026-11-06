/*
 * ChatGPT /realtime/wm data-channel protocol (data channel id 0).
 *
 * Wire format: every message is {"type":"data_message","data":"<json string>"},
 * possibly nested a few times; the inner event is {"type":..., "payload":{...}}.
 * sc_wm_decode() unwraps up to SC_WM_MAX_NESTING levels and classifies the
 * event into a flat, fixed-size struct (no heap ownership leaks to callers).
 */
#ifndef SC_WM_EVENTS_H
#define SC_WM_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC_WM_MAX_NESTING 4
#define SC_WM_TEXT_MAX 512 /* transcript text per event; longer text is truncated (text_truncated) */
#define SC_WM_ID_MAX 64

typedef enum {
    SC_WM_INVALID = 0, /* not JSON / no "type" */
    SC_WM_UNKNOWN,     /* valid event of an unhandled type; see type_name */
    SC_WM_STATE_UPDATE,
    SC_WM_CHAT_MESSAGE_DELTA,
    SC_WM_SPAWN_UPDATE,
    SC_WM_USAGE_UPDATE,
    SC_WM_GOODBYE,
    SC_WM_CLOSE_READY,
    SC_WM_CLOSE_REQUEST,
    SC_WM_THINKING_UPDATE,
    SC_WM_SESSION_BOOTSTRAP,
    SC_WM_CONVERSATION_UPDATE,
    SC_WM_STARTUP_TELEMETRY,
    SC_WM_ACTION_REQUEST,
    SC_WM_USER_TRANSCRIPTION, /* user_transcription_text */
    SC_WM_LIVE_CAPTION,       /* live_captioning_text */
} sc_wm_type_t;

typedef enum {
    SC_RT_UNKNOWN = 0,
    SC_RT_CONNECTING,
    SC_RT_IDLE,
    SC_RT_CONNECTED,
    SC_RT_HALTED,
    SC_RT_LISTENING,
    SC_RT_LISTENING_INTENTLY,
    SC_RT_THINKING,
    SC_RT_SPEAKING,
} sc_rt_state_t;

typedef enum { SC_ROLE_UNKNOWN = 0, SC_ROLE_USER, SC_ROLE_ASSISTANT, SC_ROLE_TOOL, SC_ROLE_SYSTEM } sc_wm_role_t;

typedef enum { SC_MSG_STATUS_NONE = 0, SC_MSG_STATUS_IN_PROGRESS, SC_MSG_STATUS_FINISHED, SC_MSG_STATUS_OTHER } sc_msg_status_t;

typedef enum {
    SC_TEXT_NONE = 0,
    SC_TEXT_SET,    /* "add" of a new message: text is the full initial text */
    SC_TEXT_APPEND, /* patch: append text to the current message */
} sc_text_op_t;

typedef struct {
    sc_wm_type_t type;
    char type_name[32];

    /* state_update */
    sc_rt_state_t new_state;
    sc_rt_state_t previous_state;

    /* chat_message_delta (also user_transcription_text / live_captioning_text -> text, SC_TEXT_SET) */
    struct {
        int index;           /* "c"; -1 when absent (applies to the current message) */
        bool is_add;
        sc_wm_role_t role;   /* from author.role on add; filled from the tracker on patches */
        char author_name[32];/* e.g. "web.run" for tool messages */
        char message_id[SC_WM_ID_MAX];
        sc_text_op_t text_op;
        char text[SC_WM_TEXT_MAX];
        bool text_truncated;
        sc_msg_status_t status;
        bool direction_out;  /* audio_transcription part with "direction":"out" (assistant speech) */
    } msg;

    /* spawn_update */
    bool spawn_start;
    bool spawn_end;
    char spawn_kind[24];

    /* usage_update */
    bool has_session_remaining;
    double session_remaining_s;
    bool has_audio_remaining;
    double audio_remaining_s;
    double session_s;
    double audio_s;
    bool hang_up;          /* instructions.hang_up == true */
    bool reconnect;        /* instructions.reconnect is non-null */
    double reconnect_delay_s; /* if reconnect was a number or {delay_s|delay|after_s: n}; else 0 */

    /* conversation_update / session_bootstrap (best effort) */
    char conversation_id[SC_WM_ID_MAX];

    /* action_request (incoming) */
    char action[32];
} sc_wm_event_t;

/** Remembers the role of the current message so text patches can be attributed. */
typedef struct {
    int index;
    sc_wm_role_t role;
    char author_name[32];
    bool direction_out;
} sc_wm_tracker_t;

void sc_wm_tracker_init(sc_wm_tracker_t *t);

/**
 * Decodes one data-channel message (text or binary payload, not necessarily
 * NUL-terminated). tracker may be NULL. Returns out->type (SC_WM_INVALID on garbage).
 */
sc_wm_type_t sc_wm_decode(const char *data, size_t len, sc_wm_tracker_t *tracker, sc_wm_event_t *out);

sc_rt_state_t sc_rt_state_from_string(const char *s);
const char *sc_rt_state_name(sc_rt_state_t s);

/* ---- outgoing (all return malloc'd, NUL-terminated wire strings; free() them) ---- */

/** {"type":"data_message","data":"{\"type\":\"action_request\",\"payload\":{\"action\":\"stop_speaking\"}}"} */
char *sc_wm_encode_action(const char *action);

/**
 * relay_message (typed text injected into the live conversation), as
 * createChatGPTRealtimeRelayMessage(): message {id, author{role:user}, create_time,
 * content{content_type:text, parts:[text]}, metadata{}}.
 */
char *sc_wm_encode_relay_message(const char *text, const char *message_id, double create_time_s);

/** Wraps any inner event JSON object string in the data_message envelope. */
char *sc_wm_wrap(const char *inner_event_json);

#ifdef __cplusplus
}
#endif
#endif
