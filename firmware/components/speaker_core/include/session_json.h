/*
 * The multipart "session" JSON for ChatGPT /realtime/wm, identical to
 * login-with-chatgpt's buildChatGPTRealtimeSession({transport:"wm", ...}),
 * with an optional overrides object (remote config `chatgpt.session_overrides`)
 * merged on top like `{...session, ...overrides}` (shallow; existing keys keep
 * their position, new keys are appended). Overrides cannot replace
 * voice_session_id / voice_status_request_id.
 */
#ifndef SC_SESSION_JSON_H
#define SC_SESSION_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *voice;           /* NULL -> "juniper" */
    const char *language;        /* BCP 47 tag, e.g. "de-DE"; NULL -> JSON null */
    const char *timezone;        /* IANA name; NULL -> "UTC" */
    int timezone_offset_min;     /* JS Date#getTimezoneOffset() convention: UTC - local, e.g. -120 for CEST */
    const char *conversation_id; /* NULL -> null (new conversation) */
    const char *voice_session_id;/* required: fresh UUID per call (sc_uuid_v4); also used as voice_status_request_id */
    const char *overrides_json;  /* NULL or a JSON object string; invalid JSON is ignored */
} sc_session_opts_t;

/**
 * Returns a malloc'd compact JSON string (free with free()), or NULL on
 * invalid arguments (missing voice_session_id, empty/overlong voice or
 * language, |offset| > 1440) or OOM.
 */
char *sc_session_json_build(const sc_session_opts_t *opts);

#ifdef __cplusplus
}
#endif
#endif
