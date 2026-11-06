#include "gls_wg.h"

#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "sdkconfig.h"

static const char *TAG = "wg";

#if CONFIG_GLS_WG_ENABLE
#include "esp_wireguard.h"

#define STALE_S 180          // WireGuard rekeys every 120 s; no handshake for 3 min = tunnel is dead
#define CHECK_PERIOD_MS 10000

static wireguard_config_t s_cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
static wireguard_ctx_t s_ctx = ESP_WIREGUARD_CONTEXT_DEFAULT();
static struct ifreq s_ifr = {.ifr_name = "wg?"}; // nonexistent until ready; fail closed
static volatile bool s_up;
static volatile bool s_started;
static bool s_initialized;
static bool s_connected;
static int s_down_s;

static esp_err_t wg_connect(void)
{
    s_cfg.private_key = CONFIG_GLS_WG_PRIVATE_KEY;
    s_cfg.public_key = CONFIG_GLS_WG_PEER_PUBLIC_KEY;
    s_cfg.preshared_key = strlen(CONFIG_GLS_WG_PRESHARED_KEY) ? CONFIG_GLS_WG_PRESHARED_KEY : NULL;
    s_cfg.address = CONFIG_GLS_WG_ADDRESS;
    s_cfg.netmask = CONFIG_GLS_WG_NETMASK;
    s_cfg.endpoint = CONFIG_GLS_WG_ENDPOINT;
    s_cfg.port = CONFIG_GLS_WG_PORT;
    s_cfg.persistent_keepalive = CONFIG_GLS_WG_KEEPALIVE_S;

    esp_err_t err = ESP_OK;
    if (!s_initialized) {
        err = esp_wireguard_init(&s_cfg, &s_ctx);
        s_initialized = (err == ESP_OK);
    }
    if (err == ESP_OK) {
        err = esp_wireguard_connect(&s_ctx); // resolves the endpoint (DDNS) on every (re)connect
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect to %s:%d failed: %s", CONFIG_GLS_WG_ENDPOINT, CONFIG_GLS_WG_PORT, esp_err_to_name(err));
        return err;
    }
    // Only the relay host goes through the tunnel; the default route stays on Wi-Fi.
    err = esp_wireguard_add_allowed_ip(&s_ctx, CONFIG_GLS_WG_ALLOWED_IP, "255.255.255.255");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot allow relay address %s: %s", CONFIG_GLS_WG_ALLOWED_IP, esp_err_to_name(err));
        return err;
    }
    memset(&s_ifr, 0, sizeof(s_ifr));
    snprintf(s_ifr.ifr_name, sizeof(s_ifr.ifr_name), "%c%c%d", s_ctx.netif->name[0], s_ctx.netif->name[1],
             s_ctx.netif->num);
    ESP_LOGI(TAG, "tunnel interface %s, %s -> %s:%d (relay host %s)", s_ifr.ifr_name, CONFIG_GLS_WG_ADDRESS,
             CONFIG_GLS_WG_ENDPOINT, CONFIG_GLS_WG_PORT, CONFIG_GLS_WG_ALLOWED_IP);
    return ESP_OK;
}

static bool handshake_fresh(void)
{
    if (esp_wireguard_peer_is_up(&s_ctx) != ESP_OK) {
        return false;
    }
    time_t last = 0;
    if (esp_wireguard_latest_handshake(&s_ctx, &last) != ESP_OK || last == 0) {
        return false;
    }
    return time(NULL) - last < STALE_S;
}

// The component uses lwIP's raw API. Serialize all access with TCP/IP processing, including
// interface teardown; calling it directly from a FreeRTOS worker races packet handling.
static void step_on_tcpip(void *arg)
{
    (void)arg;
    if (!s_connected) {
        // A first DNS lookup may be asynchronous. Retry on the existing interface instead
        // of reinitializing (which loses/leaks it and can register the same netif twice).
        esp_err_t err = wg_connect();
        s_connected = (err == ESP_OK);
        if (!s_connected && err != ESP_ERR_RETRY && s_ctx.netif) {
            esp_wireguard_disconnect(&s_ctx);
        }
    }
    bool up = s_connected && handshake_fresh();
    if (up != s_up) {
        ESP_LOGI(TAG, "tunnel %s", up ? "UP (handshake ok)" : "DOWN");
        s_up = up;
    }
    s_down_s = up ? 0 : s_down_s + CHECK_PERIOD_MS / 1000;
    if (s_connected && !up && s_down_s >= 60) {
        ESP_LOGW(TAG, "no handshake for %d s, reconnecting", s_down_s);
        strlcpy(s_ifr.ifr_name, "wg?", sizeof(s_ifr.ifr_name));
        esp_wireguard_disconnect(&s_ctx);
        s_connected = false;
        s_down_s = 0;
    }
}

static void supervisor(void *arg)
{
    (void)arg;
    for (;;) {
        tcpip_callback_wait(step_on_tcpip, NULL);
        vTaskDelay(pdMS_TO_TICKS(CHECK_PERIOD_MS));
    }
}

esp_err_t gls_wg_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (!strlen(CONFIG_GLS_WG_PRIVATE_KEY) || !strlen(CONFIG_GLS_WG_PEER_PUBLIC_KEY) || !strlen(CONFIG_GLS_WG_ENDPOINT)) {
        ESP_LOGE(TAG, "WireGuard enabled but keys/endpoint not set");
        return ESP_ERR_INVALID_STATE;
    }
    s_started = xTaskCreatePinnedToCoreWithCaps(supervisor, "wg_sup", 4096, NULL, 4, NULL, tskNO_AFFINITY,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;  // stack in PSRAM (no flash writes)
    return s_started ? ESP_OK : ESP_ERR_NO_MEM;
}

bool gls_wg_is_up(void)
{
    return s_up;
}

void gls_wg_bind(esp_http_client_config_t *cfg)
{
    cfg->if_name = &s_ifr;
}

#else

esp_err_t gls_wg_start(void)
{
    return ESP_OK;
}

bool gls_wg_is_up(void)
{
    return false;
}

void gls_wg_bind(esp_http_client_config_t *cfg)
{
    (void)cfg;
}

#endif
