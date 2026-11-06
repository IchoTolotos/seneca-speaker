#include "chatgpt_wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "gls_http.h"
#include "multipart.h"
#include "sc_port.h"
#include "sc_util.h"
#include "session_json.h"

static const char *TAG = "chatgpt_wm";

#define WM_PATH "/realtime/wm"

/** JS Date#getTimezoneOffset() convention: UTC - local in minutes (-120 for CEST). */
static int js_timezone_offset_min(void)
{
    time_t now = time(NULL);
    struct tm lt, ut;
    localtime_r(&now, &lt);
    gmtime_r(&now, &ut);
    lt.tm_isdst = 0; // compare wall-clock fields only
    ut.tm_isdst = 0;
    double diff_s = difftime(mktime(&lt), mktime(&ut)); // local - UTC
    return (int)(-diff_s / 60);
}

static char *build_session_json(const char *conversation_id)
{
    char vsid[SC_UUID_STR_LEN];
    sc_uuid_v4(vsid, gls_sc_random, NULL);
    sc_session_opts_t o = {
        .voice = CONFIG_GLS_VOICE[0] ? CONFIG_GLS_VOICE : NULL,
        .language = CONFIG_GLS_LANGUAGE[0] ? CONFIG_GLS_LANGUAGE : NULL,
        .timezone = CONFIG_GLS_TIMEZONE[0] ? CONFIG_GLS_TIMEZONE : NULL,
        .timezone_offset_min = js_timezone_offset_min(),
        .conversation_id = conversation_id && conversation_id[0] ? conversation_id : NULL,
        .voice_session_id = vsid,
        .overrides_json = CONFIG_GLS_SESSION_OVERRIDES[0] ? CONFIG_GLS_SESSION_OVERRIDES : NULL,
    };
    return sc_session_json_build(&o);
}

esp_err_t chatgpt_wm_create_call(const chatgpt_wm_request_t *req, chatgpt_wm_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->status = -1;
    out->fail = SC_FAIL_NETWORK;
    if (!req || !req->access_token || !req->offer_sdp) {
        out->fail = SC_FAIL_OTHER;
        return ESP_ERR_INVALID_ARG;
    }

    char *session = build_session_json(req->conversation_id);
    char boundary[SC_MULTIPART_BOUNDARY_MAX], ctype[128];
    sc_multipart_make_boundary(boundary, gls_sc_random, NULL);
    sc_multipart_content_type(boundary, ctype, sizeof(ctype));
    char *body = NULL;
    size_t body_len = 0;
    if (!session || sc_multipart_build(boundary, req->offer_sdp, session, &body, &body_len) != SC_OK) {
        free(session);
        free(body);
        out->fail = SC_FAIL_OTHER;
        return ESP_ERR_NO_MEM;
    }
    free(session);

    const char *base = CONFIG_GLS_CHATGPT_BASE_URL;
    char url[128], referer[96], session_id[SC_UUID_STR_LEN];
    snprintf(url, sizeof(url), "%s" WM_PATH "?dcid=0", base);
    snprintf(referer, sizeof(referer), "%s/", base);
    sc_uuid_v4(session_id, gls_sc_random, NULL);
    size_t auth_len = strlen("Bearer ") + strlen(req->access_token) + 1;
    char *bearer = malloc(auth_len);
    if (!bearer) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    snprintf(bearer, auth_len, "Bearer %s", req->access_token);
    const char *lang = CONFIG_GLS_LANGUAGE[0] ? CONFIG_GLS_LANGUAGE : "en-US";

    const gls_http_header_t headers[] = {
        {"Authorization", bearer},
        {"Content-Type", ctype},
        {"Accept", "application/sdp"},
        {"Accept-Language", CONFIG_GLS_ACCEPT_LANGUAGE},
        {"OAI-Language", lang},
        {"OAI-Device-Id", req->device_id && req->device_id[0] ? req->device_id : NULL},
        {"OAI-Client-Version", CONFIG_GLS_CLIENT_VERSION},
        {"OAI-Client-Build-Number", CONFIG_GLS_CLIENT_BUILD},
        {"OAI-Session-Id", session_id},
        {"X-OpenAI-Target-Path", WM_PATH},
        {"X-OpenAI-Target-Route", WM_PATH},
        {"Origin", base},
        {"Referer", referer},
        {"chatgpt-account-id", req->account_id && req->account_id[0] ? req->account_id : NULL},
    };
    ESP_LOGI(TAG, "POST %s (multipart %u B, offer %u B%s)", gls_http_log_url(url), (unsigned)body_len,
             (unsigned)strlen(req->offer_sdp), req->conversation_id ? ", continuing conversation" : "");

    gls_http_resp_t resp = {.body_limit = 32 * 1024};
    esp_err_t err = gls_http_request(url, HTTP_METHOD_POST, headers, sizeof(headers) / sizeof(headers[0]),
                                     body, body_len, &resp);
    memset(bearer, 0, auth_len);
    free(bearer);
    free(body);
    if (err != ESP_OK) {
        return err; // NETWORK
    }
    out->status = resp.status;
    strlcpy(out->content_type, resp.content_type, sizeof(out->content_type));
    strlcpy(out->cf_mitigated, resp.cf_mitigated, sizeof(out->cf_mitigated));

    const char *b = resp.body ? resp.body : "";
    while (*b == ' ' || *b == '\r' || *b == '\n' || *b == '\t') {
        b++;
    }
    if (resp.status >= 200 && resp.status < 300 && strncmp(b, "v=0", 3) == 0) {
        out->answer_sdp = strdup(b);
        err = out->answer_sdp ? ESP_OK : ESP_ERR_NO_MEM;
        gls_http_resp_log(TAG, "realtime/wm", &resp, false);
    } else {
        if (resp.status == 401) {
            out->fail = SC_FAIL_UNAUTHORIZED;
        } else if (resp.status == 403 || resp.cf_mitigated[0]) {
            out->fail = SC_FAIL_EDGE;
        } else if (resp.status >= 400 && resp.status < 500) {
            out->fail = SC_FAIL_OTHER;
        } else {
            out->fail = SC_FAIL_NETWORK;
        }
        gls_http_resp_log(TAG, "realtime/wm", &resp, true);
        err = ESP_FAIL;
    }
    gls_http_resp_free(&resp);
    return err;
}

void chatgpt_wm_result_free(chatgpt_wm_result_t *res)
{
    if (res) {
        free(res->answer_sdp);
        res->answer_sdp = NULL;
    }
}
