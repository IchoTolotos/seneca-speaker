#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_client.h"

// Starts the WireGuard tunnel to the relay host (after Wi-Fi and SNTP) and a supervisor task that
// re-establishes it whenever the handshake goes stale (router/relay restart, IP change, Wi-Fi drop).
esp_err_t gls_wg_start(void);

// True when the peer has completed a handshake recently.
bool gls_wg_is_up(void);

// Fills cfg->if_name so the request leaves through the tunnel; no-op when the tunnel is disabled.
void gls_wg_bind(esp_http_client_config_t *cfg);
