// ChatGPT GPT-Live signaling: POST https://chatgpt.com/realtime/wm?dcid=0
// multipart/form-data {sdp, session} -> SDP answer. Body/session JSON come from speaker_core
// (multipart.h, session_json.h); this file adds the HTTP request and headers from
// signaling/src/chatgpt.ts.
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "session_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *access_token;    /*!< web-client JWT */
    const char *account_id;      /*!< optional, sent as chatgpt-account-id */
    const char *device_id;       /*!< OAI-Device-Id, same as the auth exchange */
    const char *offer_sdp;
    const char *conversation_id; /*!< NULL = new conversation; set on rotation to continue */
} chatgpt_wm_request_t;

typedef struct {
    int            status;
    char          *answer_sdp;   /*!< malloc'd on success */
    sc_fail_kind_t fail;         /*!< classification when not ESP_OK */
    char           content_type[96];
    char           cf_mitigated[32];
} chatgpt_wm_result_t;

/**
 * POST the offer. ESP_OK only if 2xx and the body starts with "v=0".
 * Free with chatgpt_wm_result_free() either way.
 */
esp_err_t chatgpt_wm_create_call(const chatgpt_wm_request_t *req, chatgpt_wm_result_t *out);

void chatgpt_wm_result_free(chatgpt_wm_result_t *res);

#ifdef __cplusplus
}
#endif
