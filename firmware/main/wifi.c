#include "wifi.h"

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define GOT_IP_BIT BIT0

static EventGroupHandle_t s_events;
static gls_wifi_cb_t s_cb;
static bool s_up;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        if (s_up && s_cb) {
            s_cb(false);
        }
        s_up = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, GOT_IP_BIT);
        if (!s_up && s_cb) {
            s_cb(true);
        }
        s_up = true;
    }
}

bool gls_wifi_is_up(void)
{
    return s_up;
}

esp_err_t gls_wifi_connect(gls_wifi_cb_t cb)
{
    s_cb = cb;
    if (strlen(CONFIG_GLS_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "CONFIG_GLS_WIFI_SSID is empty: run `idf.py menuconfig` -> GPT Live Speaker -> Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_GLS_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_GLS_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = strlen(CONFIG_GLS_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Latency over power: no modem sleep while streaming audio.
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_GLS_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(CONFIG_GLS_WIFI_CONNECT_TIMEOUT_MS));
    return (bits & GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void gls_time_sync(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("time.cloudflare.com", "pool.ntp.org"));
    esp_netif_sntp_init(&cfg);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        ESP_LOGI(TAG, "time synced: %s", buf);
    } else {
        ESP_LOGW(TAG, "SNTP sync timed out (TLS still works; JWT expiry checks will not)");
    }
}
