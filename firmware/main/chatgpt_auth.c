#include "chatgpt_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "gls_http.h"
#include "jwt.h"
#include "sc_port.h"
#include "sc_util.h"

static const char *TAG = "chatgpt_auth";

static struct {
    bool                 ready;
    sc_cookie_store_t    store;
    sc_session_cookies_t cookies;
    char                 device_id[SC_DEVICE_ID_MAX];
    chatgpt_token_t      token;
} s;

static void token_clear(chatgpt_token_t *t)
{
    if (t->access_token) {
        memset(t->access_token, 0, strlen(t->access_token));
        free(t->access_token);
    }
    memset(t, 0, sizeof(*t));
}

bool chatgpt_session_init(void)
{
    if (!s.ready) {
        sc_cookies_init(&s.cookies);
        sc_cookie_store_init(&s.store, gls_sc_kv());
        s.ready = true;
    }
    int rc = sc_cookie_store_load_session(&s.store, &s.cookies, s.device_id);
    if (rc != SC_OK) {
        sc_cookies_free(&s.cookies);
        s.device_id[0] = '\0';
#ifdef CONFIG_GLS_CHATGPT_DEV_SEED
        if (CONFIG_GLS_CHATGPT_DEV_SEED[0]) {
            ESP_LOGW(TAG, "no stored session (%s): seeding from CONFIG_GLS_CHATGPT_DEV_SEED (dev only)",
                     gls_sc_err_name(rc));
            if (chatgpt_session_set_credentials(CONFIG_GLS_CHATGPT_DEV_SEED, NULL) == ESP_OK) {
                return true;
            }
        }
#endif
        ESP_LOGW(TAG, "no stored ChatGPT session (%s)", gls_sc_err_name(rc));
        return false;
    }
    if (!s.device_id[0]) {
        sc_uuid_v4(s.device_id, gls_sc_random, NULL);
    }
    ESP_LOGI(TAG, "session loaded: %u cookie chunk(s)", (unsigned)s.cookies.count);
    return s.cookies.count > 0;
}

bool chatgpt_session_has_credentials(void)
{
    return s.ready && s.cookies.count > 0;
}

const char *chatgpt_session_device_id(void)
{
    return s.device_id;
}

const chatgpt_token_t *chatgpt_session_token(void)
{
    return s.token.access_token ? &s.token : NULL;
}

esp_err_t chatgpt_session_set_credentials(const char *chunk0, const char *chunk1)
{
    sc_session_cookies_t c;
    sc_cookies_init(&c);
    int rc = sc_cookies_from_setup(&c, chunk0, chunk1);
    if (rc != SC_OK || c.count == 0) {
        sc_cookies_free(&c);
        ESP_LOGE(TAG, "invalid session input (%s)", gls_sc_err_name(rc));
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.device_id[0]) {
        sc_uuid_v4(s.device_id, gls_sc_random, NULL);
    }
    rc = sc_cookie_store_save_session(&s.store, &c, s.device_id);
    if (rc != SC_OK) {
        sc_cookies_free(&c);
        ESP_LOGE(TAG, "saving session failed (%s)", gls_sc_err_name(rc));
        return ESP_FAIL;
    }
    sc_cookies_free(&s.cookies);
    s.cookies = c; // take ownership
    token_clear(&s.token);
    ESP_LOGI(TAG, "session stored: %u cookie chunk(s)", (unsigned)s.cookies.count);
    return ESP_OK;
}

void chatgpt_session_clear(void)
{
    sc_cookie_store_clear(&s.store);
    sc_cookies_free(&s.cookies);
    token_clear(&s.token);
    ESP_LOGW(TAG, "session cleared (logout)");
}

// ---------------------------------------------------------------------------

static void on_header(const char *key, const char *value, void *ctx)
{
    if (!strcasecmp(key, "set-cookie")) {
        sc_set_cookie_feed((sc_session_cookies_t *)ctx, value); // ignores non-session cookies
    }
}

static void copy_json_str(cJSON *obj, const char *key, char *dst, size_t cap)
{
    cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    if (cJSON_IsString(v) && v->valuestring) {
        strlcpy(dst, v->valuestring, cap);
    }
}

static int64_t wall_clock(const gls_http_resp_t *resp)
{
    time_t now = time(NULL);
    if (now > 1700000000) { // SNTP synced
        return (int64_t)now;
    }
    return gls_http_parse_date(resp->date);
}

void chatgpt_session_authenticate(bool force, sc_fsm_input_t *result)
{
    (void)force; // no token cache of our own: every START_AUTH hits the server
    memset(result, 0, sizeof(*result));
    result->type = SC_IN_AUTH_FAIL;
    result->fail = SC_FAIL_NETWORK;
    if (!chatgpt_session_has_credentials()) {
        result->fail = SC_FAIL_UNAUTHORIZED;
        return;
    }

    size_t hdr_len = sc_cookies_header_len(&s.cookies, s.device_id);
    char *cookie_hdr = malloc(hdr_len + 1);
    if (!cookie_hdr || sc_cookies_build_header(&s.cookies, s.device_id, cookie_hdr, hdr_len + 1) < 0) {
        free(cookie_hdr);
        return;
    }
    const char *base = CONFIG_GLS_CHATGPT_BASE_URL;
    char url[128], referer[96];
    snprintf(url, sizeof(url), "%s/api/auth/session", base);
    snprintf(referer, sizeof(referer), "%s/", base);
    const gls_http_header_t headers[] = {
        {"Accept", "application/json"},
        {"Accept-Language", CONFIG_GLS_ACCEPT_LANGUAGE},
        {"Cookie", cookie_hdr},
        {"OAI-Device-Id", s.device_id},
        {"Origin", base},
        {"Referer", referer},
    };
    ESP_LOGI(TAG, "GET %s (Cookie header %u B, %u chunk(s))", gls_http_log_url(url), (unsigned)hdr_len, (unsigned)s.cookies.count);

    sc_session_cookies_t incoming;
    sc_cookies_init(&incoming);
    gls_http_resp_t resp = {.on_header = on_header, .ctx = &incoming, .body_limit = 64 * 1024};
    esp_err_t err = gls_http_request(url, HTTP_METHOD_GET, headers, sizeof(headers) / sizeof(headers[0]),
                                     NULL, 0, &resp);
    memset(cookie_hdr, 0, hdr_len);
    free(cookie_hdr);
    if (err != ESP_OK) {
        sc_cookies_free(&incoming);
        return; // NETWORK
    }
    gls_http_resp_log(TAG, "auth/session", &resp, false);

    if (resp.status == 403 || resp.cf_mitigated[0]) {
        result->fail = SC_FAIL_EDGE;
        goto done;
    }
    if (resp.status == 401) {
        result->fail = SC_FAIL_UNAUTHORIZED;
        goto done;
    }
    if (resp.status >= 500 || resp.status < 200) {
        result->fail = SC_FAIL_NETWORK;
        goto done;
    }
    if (resp.status >= 300) {
        result->fail = SC_FAIL_OTHER;
        goto done;
    }

    if (resp.body_truncated) {
        ESP_LOGE(TAG, "session response exceeded the body limit or allocation failed");
        result->fail = SC_FAIL_NETWORK;
        goto done;
    }
    chatgpt_token_t tok = {0};
    cJSON *root = resp.body ? cJSON_Parse(resp.body) : NULL;
    if (!cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "invalid session response JSON");
        cJSON_Delete(root);
        result->fail = SC_FAIL_NETWORK;
        goto done;
    }
    cJSON *at = root ? cJSON_GetObjectItemCaseSensitive(root, "accessToken") : NULL;
    if (cJSON_IsString(at) && at->valuestring && at->valuestring[0]) {
        tok.access_token = strdup(at->valuestring);
    }
    copy_json_str(root ? cJSON_GetObjectItemCaseSensitive(root, "account") : NULL, "id", tok.account_id, sizeof(tok.account_id));
    copy_json_str(root ? cJSON_GetObjectItemCaseSensitive(root, "user") : NULL, "email", tok.email, sizeof(tok.email));
    copy_json_str(root, "expires", tok.expires, sizeof(tok.expires));
    cJSON_Delete(root);
    if (!tok.access_token || !tok.email[0]) {
        // 200 without accessToken/email: the session is dead -> needs login
        ESP_LOGE(TAG, "200 without accessToken/user.email: web session expired");
        token_clear(&tok);
        result->fail = SC_FAIL_UNAUTHORIZED;
        goto done;
    }
    if (sc_jwt_get_exp(tok.access_token, &tok.exp_unix) != SC_OK) {
        tok.exp_unix = 0;
    }
    char client_id[64] = "";
    sc_jwt_get_string_claim(tok.access_token, "client_id", client_id, sizeof(client_id));
    if (strcmp(client_id, CONFIG_GLS_WEB_CLIENT_ID) != 0) {
        // Keep going: /realtime/wm will tell (401). Logged so a changed client id is visible.
        ESP_LOGW(TAG, "token client_id '%s' != expected '%s'", client_id, CONFIG_GLS_WEB_CLIENT_ID);
    }
    tok.now_unix = wall_clock(&resp);

    // Rotation: persist BEFORE reporting success (old chunks keep working, but don't rely on it).
    int changed = sc_cookies_apply_rotation(&s.cookies, &incoming);
    if (changed == 1) {
        int rc = sc_cookie_store_save_session(&s.store, &s.cookies, s.device_id);
        if (rc != SC_OK) {
            ESP_LOGE(TAG, "rotated session NOT persisted (%s)", gls_sc_err_name(rc));
        }
    } else if (changed < 0) {
        ESP_LOGW(TAG, "rotation rejected (%s); keeping current cookies", gls_sc_err_name(changed));
    }

    token_clear(&s.token);
    s.token = tok;
    result->type = SC_IN_AUTH_OK;
    result->token_exp_unix = tok.exp_unix;
    result->now_unix = tok.now_unix;
    ESP_LOGI(TAG, "ok: %s, token %u B, valid %lld s more, cookie %s", tok.email,
             (unsigned)strlen(tok.access_token),
             (long long)(tok.exp_unix && tok.now_unix ? tok.exp_unix - tok.now_unix : -1),
             changed == 1 ? "rotated+saved" : "unchanged");

done:
    sc_cookies_free(&incoming);
    gls_http_resp_free(&resp);
}
