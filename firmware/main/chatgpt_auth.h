// ChatGPT web session (NextAuth cookie) -> web-client access token, on top of speaker_core:
//   cookies/rotation: set_cookie.h, persistence: cookie_store.h (A/B slots in NVS), JWT: jwt.h.
// Mirrors exchangeChatGPTRealtimeWebSession() in signaling/vendor/loginwithchatgpt-core.
//
// Threading: everything here is owned by ONE task (the controller's worker task).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "session_fsm.h"
#include "set_cookie.h"
#include "cookie_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char   *access_token;        /*!< malloc'd JWT; never log it */
    char    account_id[64];
    char    email[128];
    char    expires[40];         /*!< "expires" from the JSON (ISO 8601), may be empty */
    int64_t exp_unix;            /*!< JWT exp, 0 if unknown */
    int64_t now_unix;            /*!< wall clock at the response (SNTP, else HTTP Date) */
} chatgpt_token_t;

/**
 * Load the stored session. If nothing is stored and CONFIG_GLS_CHATGPT_DEV_SEED is set,
 * seed from it (dev only). Returns true if a session cookie is available.
 */
bool chatgpt_session_init(void);

bool chatgpt_session_has_credentials(void);

/** Device id (oai-did / OAI-Device-Id), stable across rotations. */
const char *chatgpt_session_device_id(void);

/** Current token, NULL if none. */
const chatgpt_token_t *chatgpt_session_token(void);

/**
 * GET /api/auth/session with the stored cookies. Applies cookie rotation and persists it
 * (before reporting success, as speaker_core/README.md requires). Fills `result` with
 * SC_IN_AUTH_OK{token_exp_unix, now_unix} or SC_IN_AUTH_FAIL{fail}; seq is left to the caller.
 */
void chatgpt_session_authenticate(bool force, sc_fsm_input_t *result);

/** Store new cookies from the setup page (chunk0/chunk1 or the Phase 0 JSON map). */
esp_err_t chatgpt_session_set_credentials(const char *chunk0, const char *chunk1);

/** Logout: erase cookies and token. */
void chatgpt_session_clear(void);

#ifdef __cplusplus
}
#endif
