#pragma once

/**
 * Milestone 1: does chatgpt.com's edge accept the ESP32 (HTTP/1.1 + mbedTLS)?
 *  (a) GET  /api/auth/session            no cookie   -> expect 200 JSON ("{}")
 *  (b) POST /realtime/wm?dcid=0   dummy bearer      -> expect 4xx JSON
 * Blocked looks like: 403 text/html + `cf-mitigated: challenge`.
 */
void gls_probe_run(void);
