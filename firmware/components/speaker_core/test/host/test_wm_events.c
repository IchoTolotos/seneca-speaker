#include <stdlib.h>

#include "cJSON.h"
#include "test.h"
#include "wm_events.h"

static sc_wm_type_t decode_fixture(const char *name, sc_wm_tracker_t *tr, sc_wm_event_t *ev)
{
    size_t len;
    char *data = sc_test_read_fixture(name, &len);
    sc_wm_type_t t = sc_wm_decode(data, len, tr, ev);
    free(data);
    return t;
}

static void state_update_single_and_nested(void)
{
    sc_wm_event_t ev;
    CHECK_EQ_INT(decode_fixture("state_update.json", NULL, &ev), SC_WM_STATE_UPDATE);
    CHECK_EQ_INT(ev.new_state, SC_RT_LISTENING);
    CHECK_EQ_INT(ev.previous_state, SC_RT_IDLE);
    CHECK_STR(ev.type_name, "state_update");
    CHECK_EQ_INT(decode_fixture("state_update_nested3.json", NULL, &ev), SC_WM_STATE_UPDATE);
    CHECK_EQ_INT(ev.new_state, SC_RT_SPEAKING);
    CHECK_EQ_INT(ev.previous_state, SC_RT_THINKING);
    /* unwrapped event and data given as an object instead of a string */
    const char *plain = "{\"type\":\"state_update\",\"new_state\":\"listening_intently\"}";
    CHECK_EQ_INT(sc_wm_decode(plain, strlen(plain), NULL, &ev), SC_WM_STATE_UPDATE);
    CHECK_EQ_INT(ev.new_state, SC_RT_LISTENING_INTENTLY);
    const char *obj = "{\"type\":\"data_message\",\"data\":{\"type\":\"state_update\",\"payload\":{\"new_state\":\"thinking\"}}}";
    CHECK_EQ_INT(sc_wm_decode(obj, strlen(obj), NULL, &ev), SC_WM_STATE_UPDATE);
    CHECK_EQ_INT(ev.new_state, SC_RT_THINKING);
}

static void transcript_stream_with_tracker(void)
{
    sc_wm_tracker_t tr;
    sc_wm_tracker_init(&tr);
    sc_wm_event_t ev;
    char transcript[256] = "";

    CHECK_EQ_INT(decode_fixture("chat_add.json", &tr, &ev), SC_WM_CHAT_MESSAGE_DELTA);
    CHECK(ev.msg.is_add);
    CHECK_EQ_INT(ev.msg.index, 1);
    CHECK_EQ_INT(ev.msg.role, SC_ROLE_ASSISTANT);
    CHECK_STR(ev.msg.message_id, "msg-abc-123");
    CHECK_EQ_INT(ev.msg.text_op, SC_TEXT_SET);
    CHECK_STR(ev.msg.text, " Loud and");
    CHECK_EQ_INT(ev.msg.status, SC_MSG_STATUS_IN_PROGRESS);
    CHECK(ev.msg.direction_out);
    strcpy(transcript, ev.msg.text);

    CHECK_EQ_INT(decode_fixture("chat_patch.json", &tr, &ev), SC_WM_CHAT_MESSAGE_DELTA);
    CHECK(!ev.msg.is_add);
    CHECK_EQ_INT(ev.msg.index, -1);
    CHECK_EQ_INT(ev.msg.text_op, SC_TEXT_APPEND);
    CHECK_STR(ev.msg.text, " clear.");
    CHECK_EQ_INT(ev.msg.role, SC_ROLE_ASSISTANT); /* from tracker */
    strcat(transcript, ev.msg.text);

    CHECK_EQ_INT(decode_fixture("chat_patch_implicit.json", &tr, &ev), SC_WM_CHAT_MESSAGE_DELTA);
    CHECK_EQ_INT(ev.msg.text_op, SC_TEXT_APPEND);
    CHECK_STR(ev.msg.text, " up?");
    strcat(transcript, ev.msg.text);
    CHECK_STR(transcript, " Loud and clear. up?");

    CHECK_EQ_INT(decode_fixture("chat_status.json", &tr, &ev), SC_WM_CHAT_MESSAGE_DELTA);
    CHECK_EQ_INT(ev.msg.status, SC_MSG_STATUS_FINISHED);
    CHECK_EQ_INT(ev.msg.text_op, SC_TEXT_NONE);

    CHECK_EQ_INT(decode_fixture("chat_tool_add.json", &tr, &ev), SC_WM_CHAT_MESSAGE_DELTA);
    CHECK_EQ_INT(ev.msg.role, SC_ROLE_TOOL);
    CHECK_STR(ev.msg.author_name, "web.run");
    CHECK_EQ_INT(ev.msg.index, 2);
    /* a following patch belongs to the tool message */
    const char *p = "{\"type\":\"chat_message_delta\",\"payload\":{\"delta\":{\"v\":\"searching\"}}}";
    sc_wm_decode(p, strlen(p), &tr, &ev);
    CHECK_EQ_INT(ev.msg.role, SC_ROLE_TOOL);
    CHECK_EQ_INT(ev.msg.text_op, SC_TEXT_APPEND);
    CHECK_STR(ev.msg.text, "searching");

    CHECK_EQ_INT(decode_fixture("chat_user_add.json", &tr, &ev), SC_WM_CHAT_MESSAGE_DELTA);
    CHECK_EQ_INT(ev.msg.role, SC_ROLE_USER);
    CHECK(!ev.msg.direction_out);
    CHECK_STR(ev.msg.text, "What's");

    /* without a tracker, patches have no role */
    CHECK_EQ_INT(decode_fixture("chat_patch.json", NULL, &ev), SC_WM_CHAT_MESSAGE_DELTA);
    CHECK_EQ_INT(ev.msg.role, SC_ROLE_UNKNOWN);
}

static void multi_op_patch_and_truncation(void)
{
    sc_wm_event_t ev;
    const char *p = "{\"type\":\"chat_message_delta\",\"payload\":{\"delta\":{\"o\":\"patch\",\"v\":["
                    "{\"p\":\"/message/content/parts/0/text\",\"o\":\"append\",\"v\":\"Hello\"},"
                    "{\"v\":\" world\"},"
                    "{\"p\":\"/message/metadata/x\",\"o\":\"replace\",\"v\":\"ignored\"},"
                    "{\"p\":\"/message/status\",\"o\":\"replace\",\"v\":\"finished_successfully\"}]}}}";
    sc_wm_decode(p, strlen(p), NULL, &ev);
    CHECK_STR(ev.msg.text, "Hello world");
    CHECK_EQ_INT(ev.msg.status, SC_MSG_STATUS_FINISHED);

    char big[2000];
    int n = snprintf(big, sizeof big, "{\"type\":\"live_captioning_text\",\"text\":\"");
    memset(big + n, 'a', 900);
    strcpy(big + n + 900, "\"}");
    CHECK_EQ_INT(sc_wm_decode(big, strlen(big), NULL, &ev), SC_WM_LIVE_CAPTION);
    CHECK(ev.msg.text_truncated);
    CHECK_EQ_INT(strlen(ev.msg.text), SC_WM_TEXT_MAX - 1);
    CHECK_EQ_INT(ev.msg.role, SC_ROLE_ASSISTANT);
}

static void spawn_usage_close(void)
{
    sc_wm_event_t ev;
    CHECK_EQ_INT(decode_fixture("spawn_start.json", NULL, &ev), SC_WM_SPAWN_UPDATE);
    CHECK(ev.spawn_start && !ev.spawn_end);
    CHECK_STR(ev.spawn_kind, "commentary");
    CHECK_EQ_INT(decode_fixture("spawn_end.json", NULL, &ev), SC_WM_SPAWN_UPDATE);
    CHECK(ev.spawn_end && !ev.spawn_start);

    CHECK_EQ_INT(decode_fixture("usage_update.json", NULL, &ev), SC_WM_USAGE_UPDATE);
    CHECK(ev.has_session_remaining && ev.has_audio_remaining);
    CHECK(ev.session_remaining_s > 7170.1 && ev.session_remaining_s < 7170.3);
    CHECK(ev.audio_remaining_s > 53970.1 && ev.audio_remaining_s < 53970.3);
    CHECK(ev.session_s > 7170.1);
    CHECK(!ev.hang_up && !ev.reconnect);

    CHECK_EQ_INT(decode_fixture("usage_update_low.json", NULL, &ev), SC_WM_USAGE_UPDATE);
    CHECK(ev.session_remaining_s > 45.4 && ev.session_remaining_s < 45.6);
    CHECK(ev.reconnect);
    CHECK(ev.reconnect_delay_s == 2);

    CHECK_EQ_INT(decode_fixture("usage_update_hangup.json", NULL, &ev), SC_WM_USAGE_UPDATE);
    CHECK(ev.hang_up);

    CHECK_EQ_INT(decode_fixture("goodbye.json", NULL, &ev), SC_WM_GOODBYE);
    CHECK_EQ_INT(decode_fixture("close_ready.json", NULL, &ev), SC_WM_CLOSE_READY);
    CHECK_EQ_INT(decode_fixture("thinking_update.json", NULL, &ev), SC_WM_THINKING_UPDATE);
    CHECK_EQ_INT(decode_fixture("conversation_update.json", NULL, &ev), SC_WM_CONVERSATION_UPDATE);
    CHECK_STR(ev.conversation_id, "68f9c0de-1111-2222-3333-444455556666");
    CHECK_EQ_INT(decode_fixture("unknown_event.json", NULL, &ev), SC_WM_UNKNOWN);
    CHECK_STR(ev.type_name, "some_future_event");
}

static void invalid_inputs(void)
{
    sc_wm_event_t ev;
    CHECK_EQ_INT(sc_wm_decode("not json", 8, NULL, &ev), SC_WM_INVALID);
    CHECK_EQ_INT(sc_wm_decode("{}", 2, NULL, &ev), SC_WM_INVALID);
    CHECK_EQ_INT(sc_wm_decode("", 0, NULL, &ev), SC_WM_INVALID);
    const char *bad = "{\"type\":\"data_message\",\"data\":\"{not json\"}";
    CHECK_EQ_INT(sc_wm_decode(bad, strlen(bad), NULL, &ev), SC_WM_INVALID);
    /* nesting deeper than the limit is rejected, not looped on */
    size_t len;
    char *deep = sc_test_read_fixture("state_update_nested3.json", &len);
    cJSON *wrap = cJSON_CreateObject();
    cJSON_AddStringToObject(wrap, "type", "data_message");
    cJSON_AddStringToObject(wrap, "data", deep);
    char *deeper = cJSON_PrintUnformatted(wrap);
    cJSON *wrap2 = cJSON_CreateObject();
    cJSON_AddStringToObject(wrap2, "type", "data_message");
    cJSON_AddStringToObject(wrap2, "data", deeper);
    char *deepest = cJSON_PrintUnformatted(wrap2);
    CHECK_EQ_INT(sc_wm_decode(deeper, strlen(deeper), NULL, &ev), SC_WM_STATE_UPDATE); /* 4 levels ok */
    CHECK_EQ_INT(sc_wm_decode(deepest, strlen(deepest), NULL, &ev), SC_WM_INVALID);  /* 5 levels */
    /* length is honoured (buffer not NUL-terminated at len) */
    char buf[128];
    const char *ok = "{\"type\":\"goodbye\"}";
    memcpy(buf, ok, strlen(ok));
    memcpy(buf + strlen(ok), "GARBAGE", 8);
    CHECK_EQ_INT(sc_wm_decode(buf, strlen(ok), NULL, &ev), SC_WM_GOODBYE);
    free(deep);
    cJSON_free(deeper);
    cJSON_free(deepest);
    cJSON_Delete(wrap);
    cJSON_Delete(wrap2);
}

static void encoders(void)
{
    char *a = sc_wm_encode_action("stop_speaking");
    CHECK_STR(a, "{\"type\":\"data_message\",\"data\":\"{\\\"type\\\":\\\"action_request\\\",\\\"payload\\\":{\\\"action\\\":\\\"stop_speaking\\\"}}\"}");
    sc_wm_event_t ev;
    CHECK_EQ_INT(sc_wm_decode(a, strlen(a), NULL, &ev), SC_WM_ACTION_REQUEST);
    CHECK_STR(ev.action, "stop_speaking");
    free(a);
    CHECK(sc_wm_encode_action("") == NULL);

    char *r = sc_wm_encode_relay_message("Wie wird das \"Wetter\"?", "id-1", 1761300000.5);
    cJSON *outer = cJSON_Parse(r);
    CHECK_STR(cJSON_GetObjectItem(outer, "type")->valuestring, "data_message");
    cJSON *inner = cJSON_Parse(cJSON_GetObjectItem(outer, "data")->valuestring);
    CHECK_STR(cJSON_GetObjectItem(inner, "type")->valuestring, "relay_message");
    cJSON *payload = cJSON_GetObjectItem(inner, "payload");
    CHECK_STR(cJSON_GetObjectItem(payload, "type")->valuestring, "relay_message");
    cJSON *msg = cJSON_GetObjectItem(payload, "message");
    CHECK_STR(cJSON_GetObjectItem(msg, "id")->valuestring, "id-1");
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "author"), "role")->valuestring, "user");
    CHECK(cJSON_GetObjectItem(msg, "create_time")->valuedouble == 1761300000.5);
    cJSON *content = cJSON_GetObjectItem(msg, "content");
    CHECK_STR(cJSON_GetObjectItem(content, "content_type")->valuestring, "text");
    CHECK_STR(cJSON_GetArrayItem(cJSON_GetObjectItem(content, "parts"), 0)->valuestring, "Wie wird das \"Wetter\"?");
    CHECK(cJSON_IsObject(cJSON_GetObjectItem(msg, "metadata")));
    cJSON_Delete(inner);
    cJSON_Delete(outer);
    free(r);
}

void suite_wm_events(void)
{
    RUN(state_update_single_and_nested);
    RUN(transcript_stream_with_tracker);
    RUN(multi_op_patch_and_truncation);
    RUN(spawn_usage_close);
    RUN(invalid_inputs);
    RUN(encoders);
}
