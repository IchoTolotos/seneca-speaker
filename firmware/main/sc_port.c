#include "sc_port.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "sc";

uint64_t gls_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint64_t clock_ms(void *ctx)
{
    (void)ctx;
    return gls_now_ms();
}

uint32_t gls_sc_random(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static void log_fn(void *ctx, sc_log_level_t level, const char *msg)
{
    (void)ctx;
    switch (level) {
    case SC_LOG_ERROR: ESP_LOGE(TAG, "%s", msg); break;
    case SC_LOG_WARN:  ESP_LOGW(TAG, "%s", msg); break;
    case SC_LOG_INFO:  ESP_LOGI(TAG, "%s", msg); break;
    default:           ESP_LOGD(TAG, "%s", msg); break;
    }
}

const sc_platform_t *gls_sc_platform(void)
{
    static const sc_platform_t p = {
        .ctx = NULL,
        .now_ms = clock_ms,
        .random_u32 = gls_sc_random,
        .log = log_fn,
    };
    return &p;
}

// ---------------------------------------------------------------------------
// KV over NVS. One handle kept open; every set commits (durable on return).
// ---------------------------------------------------------------------------

static nvs_handle_t s_nvs;
static bool s_nvs_open;

static int kv_handle(nvs_handle_t *out)
{
    if (!s_nvs_open) {
        esp_err_t err = nvs_open(GLS_SESSION_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_open(%s): %s", GLS_SESSION_NVS_NAMESPACE, esp_err_to_name(err));
            return SC_ERR_IO;
        }
        s_nvs_open = true;
    }
    *out = s_nvs;
    return SC_OK;
}

static int kv_get(void *ctx, const char *key, void *buf, size_t cap, size_t *out_len)
{
    (void)ctx;
    nvs_handle_t h;
    if (kv_handle(&h) != SC_OK) {
        return SC_ERR_IO;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return SC_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return SC_ERR_IO;
    }
    if (out_len) {
        *out_len = len;
    }
    if (buf == NULL) {
        return SC_OK;
    }
    if (cap < len) {
        return SC_ERR_TOO_BIG;
    }
    err = nvs_get_blob(h, key, buf, &len);
    return err == ESP_OK ? SC_OK : SC_ERR_IO;
}

static int kv_set(void *ctx, const char *key, const void *data, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    if (kv_handle(&h) != SC_OK) {
        return SC_ERR_IO;
    }
    esp_err_t err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs set %s (%u B): %s", key, (unsigned)len, esp_err_to_name(err));
        return SC_ERR_IO;
    }
    return SC_OK;
}

static int kv_erase(void *ctx, const char *key)
{
    (void)ctx;
    nvs_handle_t h;
    if (kv_handle(&h) != SC_OK) {
        return SC_ERR_IO;
    }
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return SC_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    return err == ESP_OK ? SC_OK : SC_ERR_IO;
}

const sc_kv_t *gls_sc_kv(void)
{
    static const sc_kv_t kv = {
        .ctx = NULL,
        .get = kv_get,
        .set = kv_set,
        .erase = kv_erase,
    };
    return &kv;
}

const char *gls_sc_err_name(int err)
{
    switch (err) {
    case SC_OK: return "OK";
    case SC_ERR_ARG: return "ARG";
    case SC_ERR_NOMEM: return "NOMEM";
    case SC_ERR_PARSE: return "PARSE";
    case SC_ERR_NOT_FOUND: return "NOT_FOUND";
    case SC_ERR_IO: return "IO";
    case SC_ERR_CORRUPT: return "CORRUPT";
    case SC_ERR_TOO_BIG: return "TOO_BIG";
    default: return "?";
    }
}
