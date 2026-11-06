#include <stdlib.h>

#include "session_json.h"
#include "test.h"

static const char *UUID = "0d6c1f9e-5b7a-4c1e-9a8b-112233445566";

static void defaults_match_login_with_chatgpt(void)
{
    sc_session_opts_t o = {0};
    o.voice_session_id = UUID;
    char *s = sc_session_json_build(&o);
    /* Same key order and values as buildChatGPTRealtimeSession({transport:"wm"}) + JSON.stringify. */
    CHECK_STR(s, "{\"conversation_id\":null,\"language_code\":null,\"requested_default_model\":\"\","
                 "\"voice\":\"juniper\",\"voice_session_id\":\"0d6c1f9e-5b7a-4c1e-9a8b-112233445566\","
                 "\"voice_status_request_id\":\"0d6c1f9e-5b7a-4c1e-9a8b-112233445566\","
                 "\"timezone_offset_min\":0,\"timezone\":\"UTC\",\"voice_mode\":\"wingman\",\"model_slug\":\"\","
                 "\"client_tools\":[],\"history_and_training_disabled\":false,"
                 "\"conversation_mode\":{\"kind\":\"primary_assistant\"},\"model_slug_advanced\":\"\","
                 "\"enable_message_streaming\":true,\"backend_reasoning_effort\":\"high\",\"chat_mode\":\"chat\"}");
    free(s);
}

static void options_and_overrides(void)
{
    sc_session_opts_t o = {0};
    o.voice_session_id = UUID;
    o.voice = "sol";
    o.language = "de-DE";
    o.timezone = "Europe/Berlin";
    o.timezone_offset_min = -120;
    o.conversation_id = "conv-1";
    o.overrides_json = "{\"voice_session_id\":\"evil\",\"chat_mode\":\"voice\",\"new_field\":{\"a\":1},\"language_code\":null}";
    char *s = sc_session_json_build(&o);
    CHECK_STR(s, "{\"conversation_id\":\"conv-1\",\"language_code\":null,\"requested_default_model\":\"\","
                 "\"voice\":\"sol\",\"voice_session_id\":\"0d6c1f9e-5b7a-4c1e-9a8b-112233445566\","
                 "\"voice_status_request_id\":\"0d6c1f9e-5b7a-4c1e-9a8b-112233445566\","
                 "\"timezone_offset_min\":-120,\"timezone\":\"Europe/Berlin\",\"voice_mode\":\"wingman\","
                 "\"model_slug\":\"\",\"client_tools\":[],\"history_and_training_disabled\":false,"
                 "\"conversation_mode\":{\"kind\":\"primary_assistant\"},\"model_slug_advanced\":\"\","
                 "\"enable_message_streaming\":true,\"backend_reasoning_effort\":\"high\",\"chat_mode\":\"voice\","
                 "\"new_field\":{\"a\":1}}");
    free(s);
}

static void invalid_inputs(void)
{
    sc_session_opts_t o = {0};
    CHECK(sc_session_json_build(&o) == NULL); /* missing uuid */
    o.voice_session_id = UUID;
    o.voice = "";
    CHECK(sc_session_json_build(&o) == NULL);
    o.voice = NULL;
    o.timezone_offset_min = 2000;
    CHECK(sc_session_json_build(&o) == NULL);
    o.timezone_offset_min = 0;
    o.overrides_json = "not json";
    char *s = sc_session_json_build(&o); /* bad overrides are ignored */
    CHECK(s != NULL && strstr(s, "\"chat_mode\":\"chat\"") != NULL);
    free(s);
    o.overrides_json = "[1,2]";
    s = sc_session_json_build(&o);
    CHECK(s != NULL && strstr(s, "[1,2]") == NULL);
    free(s);
}

void suite_session_json(void)
{
    RUN(defaults_match_login_with_chatgpt);
    RUN(options_and_overrides);
    RUN(invalid_inputs);
}
