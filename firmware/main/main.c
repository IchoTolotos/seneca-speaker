// gpt-live-speaker firmware entry point.
//   boot: NVS -> I2S/media (XVF3800 clocks start) -> Wi-Fi -> SNTP -> optional probe
//         -> controller (speaker_core FSM: auth -> /realtime/wm -> WebRTC call)
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "controller.h"
#include "xvf_ctrl.h"
#include "media.h"
#include "probe.h"
#include "wifi.h"
#include "gls_wg.h"
#include "setup.h"
#include "wake_word.h"

static const char *TAG = "gls";

// Diagnostics: every failed allocation (size, caps, caller) - starvation shows up here first.
static void alloc_failed(size_t size, uint32_t caps, const char *fn)
{
    esp_rom_printf("ALLOC FAIL %u B caps 0x%x in %s (internal free %u, largest %u)\n", (unsigned)size,
                   (unsigned)caps, fn ? fn : "?", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "[%s] heap internal free %u (largest %u, min %u), psram free %u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "gpt-live-speaker %s (IDF %s)", app->version, app->idf_ver);
    heap_caps_register_failed_alloc_callback(alloc_failed);
    // cJSON builds one small node per value (the 36 KB auth/session reply = hundreds of them): keep them out
    // of internal RAM, which otherwise drops to a few bytes during sign-in and call events.
    static cJSON_Hooks psram_hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&psram_hooks);
    log_heap("boot");

    init_nvs();
    setenv("TZ", CONFIG_GLS_TZ_POSIX, 1);
    tzset();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (gls_xvf_init() != ESP_OK) ESP_LOGW(TAG, "XVF3800 I2C control unavailable");
    if (gls_media_init() != ESP_OK) {
        ESP_LOGE(TAG, "media init failed; continuing without audio");
    }
    if (gls_wake_start() != ESP_OK) ESP_LOGE(TAG, "Seneca startup failed");
    log_heap("media up");

    if (gls_wifi_connect(gls_controller_wifi) != ESP_OK) {
        ESP_LOGE(TAG, "no Wi-Fi yet; set it with `idf.py menuconfig` (GPT Live Speaker -> Wi-Fi). Still retrying.");
    } else {
        gls_time_sync();
#if CONFIG_GLS_WG_ENABLE
        gls_wg_start();
        for (int i = 0; i < 30 && !gls_wg_is_up(); i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "relay tunnel %s", gls_wg_is_up() ? "up" : "not up yet (keeps retrying)");
#endif
#if CONFIG_GLS_RUN_PROBE
        gls_probe_run();
#endif
    }
    log_heap("wifi up");

    ESP_ERROR_CHECK(gls_controller_start());
    ESP_ERROR_CHECK(gls_setup_start());

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        log_heap("periodic");
    }
}
