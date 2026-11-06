#pragma once
#include "esp_err.h"

#include <stdbool.h>

/** Called from the event loop task on got-IP (true) / disconnect (false). Keep it short. */
typedef void (*gls_wifi_cb_t)(bool up);

/**
 * Station connect with CONFIG_GLS_WIFI_SSID/PASSWORD; blocks until IP or timeout.
 * Reconnects forever in the background; `cb` (may be NULL) reports every up/down edge.
 */
esp_err_t gls_wifi_connect(gls_wifi_cb_t cb);

bool gls_wifi_is_up(void);

/** Start SNTP and wait (bounded) for a valid wall clock (needed for JWT exp checks). */
void gls_time_sync(void);
