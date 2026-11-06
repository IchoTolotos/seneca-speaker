#include "wm_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static const struct {
    const char *name;
    sc_wm_type_t type;
} TYPES[] = {
    {"state_update", SC_WM_STATE_UPDATE},
    {"chat_message_delta", SC_WM_CHAT_MESSAGE_DELTA},
    {"spawn_update", SC_WM_SPAWN_UPDATE},
    {"usage_update", SC_WM_USAGE_UPDATE},
    {"goodbye", SC_WM_GOODBYE},
    {"close_ready", SC_WM_CLOSE_READY},
    {"close_request", SC_WM_CLOSE_REQUEST},
    {"thinking_update", SC_WM_THINKING_UPDATE},
    {"session_bootstrap", SC_WM_SESSION_BOOTSTRAP},
    {"conversation_update", SC_WM_CONVERSATION_UPDATE},
    {"startup_telemetry", SC_WM_STARTUP_TELEMETRY},
    {"action_request", SC_WM_ACTION_REQUEST},
    {"user_transcription_text", SC_WM_USER_TRANSCRIPTION},
    {"live_captioning_text", SC_WM_LIVE_CAPTION},
};

static const char *STATE_NAMES[] = {
    "unknown", "connecting", "idle", "connected", "halted", "listening", "listening_intently", "thinking", "speaking",
};

sc_rt_state_t sc_rt_state_from_string(const char *s)
{
    if (!s) return SC_RT_UNKNOWN;
    for (int i = 1; i < (int)(sizeof STATE_NAMES / sizeof STATE_NAMES[0]); i++)
        if (strcmp(s, STATE_NAMES[i]) == 0) return (sc_rt_state_t)i;
    return SC_RT_UNKNOWN;
}

const char *sc_rt_state_name(sc_rt_state_t s)
{
    if ((int)s < 0 || (int)s >= (int)(sizeof STATE_NAMES / sizeof STATE_NAMES[0])) return "unknown";
    return STATE_NAMES[s];
}

void sc_wm_tracker_init(sc_wm_tracker_t *t)
{
    memset(t, 0, sizeof(*t));
    t->index = -1;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static const char *get_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static void text_append(sc_wm_event_t *ev, const char *s)
{
    size_t cur = strlen(ev->msg.text), add = strlen(s);
    if (cur + add >= SC_WM_TEXT_MAX) {
        add = SC_WM_TEXT_MAX - 1 - cur;
        ev->msg.text_truncated = true;
    }
    memcpy(ev->msg.text + cur, s, add);
    ev->msg.text[cur + add] = 0;
}

static sc_wm_role_t role_from(const char *s)
{
    if (!s) return SC_ROLE_UNKNOWN;
    if (!strcmp(s, "user")) return SC_ROLE_USER;
    if (!strcmp(s, "assistant")) return SC_ROLE_ASSISTANT;
    if (!strcmp(s, "tool")) return SC_ROLE_TOOL;
    if (!strcmp(s, "system")) return SC_ROLE_SYSTEM;
    return SC_ROLE_UNKNOWN;
}

static sc_msg_status_t status_from(const char *s)
{
    if (!s) return SC_MSG_STATUS_NONE;
    if (!strcmp(s, "in_progress")) return SC_MSG_STATUS_IN_PROGRESS;
    if (!strcmp(s, "finished_successfully")) return SC_MSG_STATUS_FINISHED;
    return SC_MSG_STATUS_OTHER;
}

static bool is_text_path(const char *p)
{
    /* /message/content/parts/<n>/text  or  /message/content/parts/<n> (string parts) */
    static const char prefix[] = "/message/content/parts/";
    if (!p || strncmp(p, prefix, sizeof prefix - 1) != 0) return false;
    p += sizeof prefix - 1;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    return *p == 0 || strcmp(p, "/text") == 0;
}

/* Applies one JSON-patch-like op {p, o, v}. last_text tracks the implicit path. */
static void apply_op(const cJSON *op, sc_wm_event_t *ev, bool *last_text)
{
    const char *p = get_str(op, "p");
    const char *o = get_str(op, "o");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(op, "v");
    if (p) *last_text = is_text_path(p);
    if (p && !strcmp(p, "/message/status") && cJSON_IsString(v)) {
        ev->msg.status = status_from(v->valuestring);
        return;
    }
    if (!cJSON_IsString(v)) return;
    bool text_target = p ? is_text_path(p) : *last_text;
    if (!text_target) return;
    if (o && !strcmp(o, "replace")) {
        ev->msg.text[0] = 0;
        ev->msg.text_truncated = false;
        ev->msg.text_op = SC_TEXT_SET;
    } else if (o && strcmp(o, "append") != 0) {
        return;
    } else if (ev->msg.text_op == SC_TEXT_NONE) {
        ev->msg.text_op = SC_TEXT_APPEND;
    }
    text_append(ev, v->valuestring);
}

static void decode_message_add(const cJSON *message, sc_wm_event_t *ev)
{
    const cJSON *author = cJSON_GetObjectItemCaseSensitive(message, "author");
    ev->msg.role = role_from(get_str(author, "role"));
    copy_str(ev->msg.author_name, sizeof ev->msg.author_name, get_str(author, "name"));
    copy_str(ev->msg.message_id, sizeof ev->msg.message_id, get_str(message, "id"));
    ev->msg.status = status_from(get_str(message, "status"));
    ev->msg.text_op = SC_TEXT_SET;
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    const cJSON *parts = cJSON_GetObjectItemCaseSensitive(content, "parts");
    const cJSON *part;
    cJSON_ArrayForEach(part, parts)
    {
        if (cJSON_IsString(part)) {
            text_append(ev, part->valuestring);
        } else if (cJSON_IsObject(part)) {
            const char *t = get_str(part, "text");
            if (t) text_append(ev, t);
            const char *dir = get_str(part, "direction");
            if (dir && !strcmp(dir, "out")) ev->msg.direction_out = true;
        }
    }
}

static void decode_delta(const cJSON *payload, sc_wm_event_t *ev, sc_wm_tracker_t *tr)
{
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(payload, "delta");
    if (!cJSON_IsObject(delta)) delta = payload;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(delta, "c");
    ev->msg.index = cJSON_IsNumber(c) ? c->valueint : -1;
    const char *o = get_str(delta, "o");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(delta, "v");
    bool last_text = true; /* implicit path defaults to the transcript text */

    if (o && !strcmp(o, "add") && cJSON_IsObject(v)) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(v, "message");
        ev->msg.is_add = true;
        if (cJSON_IsObject(message)) decode_message_add(message, ev);
        if (tr) {
            tr->index = ev->msg.index;
            tr->role = ev->msg.role;
            tr->direction_out = ev->msg.direction_out;
            copy_str(tr->author_name, sizeof tr->author_name, ev->msg.author_name);
        }
        return;
    }
    if (cJSON_IsArray(v) && (!o || !strcmp(o, "patch"))) {
        const cJSON *op;
        cJSON_ArrayForEach(op, v)
        {
            if (cJSON_IsObject(op)) apply_op(op, ev, &last_text);
        }
    } else {
        apply_op(delta, ev, &last_text);
    }
    if (tr && (ev->msg.index < 0 || ev->msg.index == tr->index)) {
        ev->msg.role = tr->role;
        ev->msg.direction_out = tr->direction_out;
        copy_str(ev->msg.author_name, sizeof ev->msg.author_name, tr->author_name);
    }
}

static double num_or(const cJSON *obj, const char *key, bool *has)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        if (has) *has = true;
        return it->valuedouble;
    }
    return 0;
}

static void decode_usage(const cJSON *payload, sc_wm_event_t *ev)
{
    const cJSON *limits = cJSON_GetObjectItemCaseSensitive(payload, "limits");
    ev->session_remaining_s =
        num_or(cJSON_GetObjectItemCaseSensitive(limits, "session"), "remaining_seconds", &ev->has_session_remaining);
    ev->audio_remaining_s =
        num_or(cJSON_GetObjectItemCaseSensitive(limits, "audio"), "remaining_seconds", &ev->has_audio_remaining);
    ev->session_s = num_or(payload, "session_s", NULL);
    ev->audio_s = num_or(payload, "audio_s", NULL);
    const cJSON *ins = cJSON_GetObjectItemCaseSensitive(payload, "instructions");
    ev->hang_up = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ins, "hang_up"));
    const cJSON *rc = cJSON_GetObjectItemCaseSensitive(ins, "reconnect");
    if (rc && !cJSON_IsNull(rc) && !cJSON_IsFalse(rc)) {
        ev->reconnect = true;
        if (cJSON_IsNumber(rc)) {
            ev->reconnect_delay_s = rc->valuedouble;
        } else if (cJSON_IsObject(rc)) {
            const char *keys[] = {"delay_s", "delay", "after_s"};
            for (int i = 0; i < 3 && ev->reconnect_delay_s == 0; i++)
                ev->reconnect_delay_s = num_or(rc, keys[i], NULL);
        }
    }
}

static void decode_conversation_id(const cJSON *payload, const cJSON *event, sc_wm_event_t *ev)
{
    const char *id = get_str(payload, "conversation_id");
    if (!id) id = get_str(event, "conversation_id");
    if (!id) id = get_str(cJSON_GetObjectItemCaseSensitive(payload, "conversation"), "id");
    copy_str(ev->conversation_id, sizeof ev->conversation_id, id);
}

sc_wm_type_t sc_wm_decode(const char *data, size_t len, sc_wm_tracker_t *tr, sc_wm_event_t *ev)
{
    memset(ev, 0, sizeof(*ev));
    ev->msg.index = -1;
    if (!data || !len) return SC_WM_INVALID;

    cJSON *roots[SC_WM_MAX_NESTING + 1] = {0};
    int nroots = 0;
    cJSON *cur = roots[nroots++] = cJSON_ParseWithLength(data, len);
    for (int depth = 0; cur && depth < SC_WM_MAX_NESTING; depth++) {
        if (cJSON_IsString(cur)) {
            cur = roots[nroots++] = cJSON_Parse(cur->valuestring);
            continue;
        }
        const char *t = get_str(cur, "type");
        cJSON *d = cJSON_GetObjectItemCaseSensitive(cur, "data");
        if (t && !strcmp(t, "data_message") && d) {
            cur = cJSON_IsString(d) ? (roots[nroots++] = cJSON_Parse(d->valuestring)) : d;
            continue;
        }
        break;
    }
    const char *type = get_str(cur, "type");
    if (!cJSON_IsObject(cur) || !type || !strcmp(type, "data_message")) {
        ev->type = SC_WM_INVALID;
        goto done;
    }
    copy_str(ev->type_name, sizeof ev->type_name, type);
    ev->type = SC_WM_UNKNOWN;
    for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
        if (!strcmp(type, TYPES[i].name)) ev->type = TYPES[i].type;

    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(cur, "payload");
    if (!cJSON_IsObject(payload)) payload = cur;

    switch (ev->type) {
    case SC_WM_STATE_UPDATE: {
        const char *ns = get_str(payload, "new_state");
        if (!ns) ns = get_str(cur, "new_state");
        ev->new_state = sc_rt_state_from_string(ns);
        ev->previous_state = sc_rt_state_from_string(get_str(payload, "previous_state"));
        break;
    }
    case SC_WM_CHAT_MESSAGE_DELTA:
        decode_delta(payload, ev, tr);
        break;
    case SC_WM_SPAWN_UPDATE: {
        const char *st = get_str(payload, "state");
        copy_str(ev->spawn_kind, sizeof ev->spawn_kind, get_str(payload, "kind"));
        ev->spawn_start = st && !strcmp(st, "start");
        ev->spawn_end = st && !strcmp(st, "end");
        break;
    }
    case SC_WM_USAGE_UPDATE:
        decode_usage(payload, ev);
        break;
    case SC_WM_CONVERSATION_UPDATE:
    case SC_WM_SESSION_BOOTSTRAP:
        decode_conversation_id(payload, cur, ev);
        break;
    case SC_WM_ACTION_REQUEST:
        copy_str(ev->action, sizeof ev->action, get_str(payload, "action"));
        break;
    case SC_WM_USER_TRANSCRIPTION:
    case SC_WM_LIVE_CAPTION: {
        const char *t = get_str(payload, "text");
        if (!t) t = get_str(payload, "transcript");
        if (t) {
            ev->msg.text_op = SC_TEXT_SET;
            text_append(ev, t);
        }
        ev->msg.role = ev->type == SC_WM_USER_TRANSCRIPTION ? SC_ROLE_USER : SC_ROLE_ASSISTANT;
        break;
    }
    default:
        break;
    }
done:
    for (int i = 0; i < nroots; i++) cJSON_Delete(roots[i]);
    return ev->type;
}

/* ---- encoders ---- */

char *sc_wm_wrap(const char *inner)
{
    if (!inner) return NULL;
    cJSON *outer = cJSON_CreateObject();
    if (!outer) return NULL;
    cJSON_AddStringToObject(outer, "type", "data_message");
    cJSON_AddStringToObject(outer, "data", inner);
    char *s = cJSON_PrintUnformatted(outer);
    cJSON_Delete(outer);
    return s;
}

static char *wrap_obj(cJSON *inner)
{
    if (!inner) return NULL;
    char *s = cJSON_PrintUnformatted(inner);
    cJSON_Delete(inner);
    if (!s) return NULL;
    char *w = sc_wm_wrap(s);
    cJSON_free(s);
    return w;
}

char *sc_wm_encode_action(const char *action)
{
    if (!action || !*action) return NULL;
    cJSON *ev = cJSON_CreateObject();
    if (!ev) return NULL;
    cJSON_AddStringToObject(ev, "type", "action_request");
    cJSON *p = cJSON_AddObjectToObject(ev, "payload");
    if (p) cJSON_AddStringToObject(p, "action", action);
    return wrap_obj(ev);
}

char *sc_wm_encode_relay_message(const char *text, const char *message_id, double create_time_s)
{
    if (!text || !message_id) return NULL;
    cJSON *ev = cJSON_CreateObject();
    if (!ev) return NULL;
    cJSON_AddStringToObject(ev, "type", "relay_message");
    cJSON *p = cJSON_AddObjectToObject(ev, "payload");
    cJSON_AddStringToObject(p, "type", "relay_message");
    cJSON *m = cJSON_AddObjectToObject(p, "message");
    cJSON_AddStringToObject(m, "id", message_id);
    cJSON *author = cJSON_AddObjectToObject(m, "author");
    cJSON_AddStringToObject(author, "role", "user");
    cJSON_AddNumberToObject(m, "create_time", create_time_s);
    cJSON *content = cJSON_AddObjectToObject(m, "content");
    cJSON_AddStringToObject(content, "content_type", "text");
    cJSON *parts = cJSON_AddArrayToObject(content, "parts");
    cJSON_AddItemToArray(parts, cJSON_CreateString(text));
    cJSON_AddObjectToObject(m, "metadata");
    return wrap_obj(ev);
}
