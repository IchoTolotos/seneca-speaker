// ESP-IDF implementations of speaker_core's injected platform interfaces (sc_platform.h):
// monotonic clock (esp_timer), random (esp_random), logging (ESP_LOGx) and KV over NVS.
#pragma once

#include "esp_err.h"
#include "sc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/** NVS namespace used for the cookie store (sc_ck_a / sc_ck_b / sc_ck_act). */
#define GLS_SESSION_NVS_NAMESPACE "gls_sess"

/** Platform bundle (clock, random, log). Static, always valid. */
const sc_platform_t *gls_sc_platform(void);

/** KV backed by NVS namespace GLS_SESSION_NVS_NAMESPACE. Opens the namespace on first use. */
const sc_kv_t *gls_sc_kv(void);

uint64_t gls_now_ms(void);
uint32_t gls_sc_random(void *ctx);

/** Map an SC_ERR_* code to a short string for logs. */
const char *gls_sc_err_name(int err);

#ifdef __cplusplus
}
#endif
