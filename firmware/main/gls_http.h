// Thin esp_http_client wrapper for chatgpt.com: HTTP/1.1 over TLS (cert bundle),
// browser User-Agent, large buffers, captures diagnostics headers and the body.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const char *value;
} gls_http_header_t;

/** Called for every response header (key/value are complete, NUL-terminated). */
typedef void (*gls_http_header_cb_t)(const char *key, const char *value, void *ctx);

typedef struct {
    // ---- in
    size_t               body_limit;  /*!< max body bytes kept (0 = 64 KB) */
    gls_http_header_cb_t on_header;   /*!< optional */
    void                *ctx;
    // ---- out
    int    status;
    char   content_type[96];
    char   cf_mitigated[32];
    char   cf_ray[48];
    char   server[32];
    char   date[40];                    /*!< HTTP Date header (clock fallback before SNTP) */
    char  *body;                      /*!< NUL-terminated, malloc'd (PSRAM for big bodies) */
    size_t body_len;
    bool   body_truncated;
} gls_http_resp_t;

/**
 * Perform one request. `body` may be NULL for GET. Returns ESP_OK if an HTTP response
 * was received (any status); transport errors are returned as esp_err_t.
 */
// URL for logs: the configured base URL (may contain the relay token) is shown as "<base>".
const char *gls_http_log_url(const char *url);

esp_err_t gls_http_request(const char *url, esp_http_client_method_t method,
                           const gls_http_header_t *headers, size_t n_headers,
                           const char *body, size_t body_len, gls_http_resp_t *resp);

void gls_http_resp_free(gls_http_resp_t *resp);

/** Log status + edge diagnostics, and a short body preview if `show_body` (never for secrets). */
void gls_http_resp_log(const char *tag, const char *what, const gls_http_resp_t *resp, bool show_body);

/** Parse an RFC 7231 IMF-fixdate ("Sun, 26 Oct 2026 10:00:00 GMT") to unix seconds; 0 on error. */
int64_t gls_http_parse_date(const char *date);

#ifdef __cplusplus
}
#endif
