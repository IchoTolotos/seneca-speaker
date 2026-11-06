#include "probe.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "gls_http.h"
#include "esp_http_client.h"
#include "chatgpt_wm.h"

static const char *TAG = "probe";

// Minimal syntactically plausible offer; the server should reject the dummy bearer before
// looking at it. What matters is *how* it rejects (JSON 401 vs. HTML challenge).
static const char *DUMMY_OFFER =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtpmap:111 opus/48000/2\r\n";

static const char *verdict(int status, const char *ctype, const char *cf_mitigated)
{
    if (cf_mitigated[0]) {
        return "BLOCKED (cf-mitigated)";
    }
    if (status == 403 && strstr(ctype, "text/html")) {
        return "BLOCKED? (403 html)";
    }
    if (strstr(ctype, "json")) {
        return "REACHED API (json)";
    }
    return "UNCLEAR";
}

static void probe_session(void)
{
    char url[128], referer[96];
    snprintf(url, sizeof(url), "%s/api/auth/session", CONFIG_GLS_CHATGPT_BASE_URL);
    snprintf(referer, sizeof(referer), "%s/", CONFIG_GLS_CHATGPT_BASE_URL);
    const gls_http_header_t headers[] = {
        {"Accept", "application/json"},
        {"Accept-Language", CONFIG_GLS_ACCEPT_LANGUAGE},
        {"Origin", CONFIG_GLS_CHATGPT_BASE_URL},
        {"Referer", referer},
    };
    gls_http_resp_t resp = {.body_limit = 4096};
    esp_err_t err = gls_http_request(url, HTTP_METHOD_GET, headers, sizeof(headers) / sizeof(headers[0]),
                                     NULL, 0, &resp);
    if (err == ESP_OK) {
        gls_http_resp_log(TAG, "(a) GET /api/auth/session (no cookie)", &resp, true);
        ESP_LOGW(TAG, "(a) verdict: %s", verdict(resp.status, resp.content_type, resp.cf_mitigated));
    } else {
        ESP_LOGE(TAG, "(a) transport error: %s", esp_err_to_name(err));
    }
    gls_http_resp_free(&resp);
}

// Cloudflare's own trace endpoint: tells us what the edge sees (client IP, TLS version, HTTP
// version, colo). Lets us separate "this IP is flagged" from "this TLS/HTTP client is flagged".
static void probe_trace(void)
{
    char url[128];
    snprintf(url, sizeof(url), "%s/cdn-cgi/trace", CONFIG_GLS_CHATGPT_BASE_URL);
    gls_http_resp_t resp = {.body_limit = 1024};
    esp_err_t err = gls_http_request(url, HTTP_METHOD_GET, NULL, 0, NULL, 0, &resp);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "(t) GET /cdn-cgi/trace -> HTTP %d\n%.*s", resp.status, (int)resp.body_len,
                 resp.body ? resp.body : "");
    } else {
        ESP_LOGE(TAG, "(t) transport error: %s", esp_err_to_name(err));
    }
    gls_http_resp_free(&resp);
}

static void probe_wm(void)
{
    chatgpt_wm_request_t req = {
        .access_token = "dummy-token-from-esp32-probe",
        .offer_sdp = DUMMY_OFFER,
    };
    chatgpt_wm_result_t res;
    esp_err_t err = chatgpt_wm_create_call(&req, &res);
    if (res.status > 0) {
        ESP_LOGW(TAG, "(b) POST /realtime/wm: HTTP %d, verdict: %s (fail class %d)", res.status,
                 verdict(res.status, res.content_type, res.cf_mitigated), (int)res.fail);
    } else {
        ESP_LOGE(TAG, "(b) transport error: %s", esp_err_to_name(err));
    }
    chatgpt_wm_result_free(&res);
}

void gls_probe_run(void)
{
    ESP_LOGI(TAG, "=== chatgpt.com reachability probe (HTTP/1.1, esp_http_client + mbedTLS) ===");
    probe_trace();
    probe_session();
    probe_wm();
    ESP_LOGI(TAG, "heap after probe: internal free %u, psram free %u, internal min %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "=== probe done ===");
}
