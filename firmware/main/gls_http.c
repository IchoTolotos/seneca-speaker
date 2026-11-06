#include "gls_http.h"
#include "gls_wg.h"
#include <string.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"

static const char *TAG = "gls_http";

#define DEFAULT_BODY_LIMIT (64 * 1024)

static void copy_hdr(char *dst, size_t cap, const char *v)
{
    if (v) {
        strlcpy(dst, v, cap);
    }
}

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    gls_http_resp_t *resp = (gls_http_resp_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER: {
        const char *k = evt->header_key;
        const char *v = evt->header_value;
        if (!k || !v) {
            break;
        }
        if (!strcasecmp(k, "content-type")) {
            copy_hdr(resp->content_type, sizeof(resp->content_type), v);
        } else if (!strcasecmp(k, "cf-mitigated")) {
            copy_hdr(resp->cf_mitigated, sizeof(resp->cf_mitigated), v);
        } else if (!strcasecmp(k, "cf-ray")) {
            copy_hdr(resp->cf_ray, sizeof(resp->cf_ray), v);
        } else if (!strcasecmp(k, "server")) {
            copy_hdr(resp->server, sizeof(resp->server), v);
        } else if (!strcasecmp(k, "date")) {
            copy_hdr(resp->date, sizeof(resp->date), v);
        }
        if (resp->on_header) {
            resp->on_header(k, v, resp->ctx);
        }
        break;
    }
    case HTTP_EVENT_ON_DATA: {
        size_t limit = resp->body_limit ? resp->body_limit : DEFAULT_BODY_LIMIT;
        size_t room = resp->body_len < limit ? limit - resp->body_len : 0;
        size_t n = (size_t)evt->data_len;
        if (n > room) {
            n = room;
            resp->body_truncated = true;
        }
        if (n == 0) {
            break;
        }
        char *nb = realloc(resp->body, resp->body_len + n + 1);
        if (!nb) {
            ESP_LOGE(TAG, "body realloc failed (%u bytes)", (unsigned)(resp->body_len + n + 1));
            resp->body_truncated = true;
            break;
        }
        resp->body = nb;
        memcpy(resp->body + resp->body_len, evt->data, n);
        resp->body_len += n;
        resp->body[resp->body_len] = '\0';
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t gls_http_request(const char *url, esp_http_client_method_t method,
                           const gls_http_header_t *headers, size_t n_headers,
                           const char *body, size_t body_len, gls_http_resp_t *resp)
{
    resp->status = -1;
    resp->content_type[0] = resp->cf_mitigated[0] = resp->cf_ray[0] = resp->server[0] = resp->date[0] = '\0';
    resp->body = NULL;
    resp->body_len = 0;
    resp->body_truncated = false;

    // Announce HTTP/1.1 via ALPN like browsers/curl do; mbedTLS sends no ALPN by default, which makes
    // the ClientHello stand out to Cloudflare's bot detection.
    static const char *alpn[] = {"http/1.1", NULL};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = CONFIG_GLS_HTTP_TIMEOUT_MS,
        .buffer_size = CONFIG_GLS_HTTP_RX_BUF,
        .buffer_size_tx = CONFIG_GLS_HTTP_TX_BUF,
        .user_agent = CONFIG_GLS_USER_AGENT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .alpn_protos = alpn,
        .event_handler = on_event,
        .user_data = resp,
        .disable_auto_redirect = true, // we want to see 3xx from the edge, not follow them
        .keep_alive_enable = false,
    };
    if (strncmp(url, CONFIG_GLS_CHATGPT_BASE_URL, strlen(CONFIG_GLS_CHATGPT_BASE_URL)) == 0) {
        gls_wg_bind(&cfg); // relay requests leave through the WireGuard tunnel (no-op if disabled)
    }
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < n_headers; i++) {
        if (headers[i].key && headers[i].value) {
            esp_http_client_set_header(client, headers[i].key, headers[i].value);
        }
    }
    if (body) {
        esp_http_client_set_post_field(client, body, (int)body_len);
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        resp->status = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGE(TAG, "%s failed: %s", gls_http_log_url(url), esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

void gls_http_resp_free(gls_http_resp_t *resp)
{
    if (resp) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
}

void gls_http_resp_log(const char *tag, const char *what, const gls_http_resp_t *resp, bool show_body)
{
    ESP_LOGI(tag, "%s -> HTTP %d | content-type: %s | cf-mitigated: %s | server: %s | cf-ray: %s | body %u B%s",
             what, resp->status,
             resp->content_type[0] ? resp->content_type : "-",
             resp->cf_mitigated[0] ? resp->cf_mitigated : "-",
             resp->server[0] ? resp->server : "-",
             resp->cf_ray[0] ? resp->cf_ray : "-",
             (unsigned)resp->body_len, resp->body_truncated ? " (truncated)" : "");
    if (show_body && resp->body_len) {
        char preview[241];
        size_t n = resp->body_len < sizeof(preview) - 1 ? resp->body_len : sizeof(preview) - 1;
        for (size_t i = 0; i < n; i++) {
            char c = resp->body[i];
            preview[i] = isprint((unsigned char)c) ? c : ' ';
        }
        preview[n] = '\0';
        ESP_LOGI(tag, "%s body: %s", what, preview);
    }
}

int64_t gls_http_parse_date(const char *date)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char wd[4], mon[4];
    int day, year, hh, mm, ss;
    if (!date || sscanf(date, "%3s, %d %3s %d %d:%d:%d", wd, &day, mon, &year, &hh, &mm, &ss) != 7) {
        return 0;
    }
    const char *p = strstr(months, mon);
    if (!p || (p - months) % 3) {
        return 0;
    }
    int m = (int)(p - months) / 3 + 1;
    // days from civil (Howard Hinnant), proleptic Gregorian, UTC
    int y = year - (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

const char *gls_http_log_url(const char *url)
{
    static char buf[160];
    size_t n = strlen(CONFIG_GLS_CHATGPT_BASE_URL);
    if (url && strncmp(url, CONFIG_GLS_CHATGPT_BASE_URL, n) == 0) {
        snprintf(buf, sizeof(buf), "<base>%s", url + n);
        return buf;
    }
    return url;
}
