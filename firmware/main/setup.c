#include "setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "controller.h"
#include "chime.h"
#include "rtc_peer.h"
#include "gls_wg.h"
#include "wake_word.h"
#include "audio_diag.h"
#include "wifi.h"
#include "xvf3800_i2s.h"
#include "airplay_tx.h"
#include "airplay_measure.h"
#include "xvf_ctrl.h"
#include <math.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static gls_ap_speaker_t s_ap_found[12];  // last AirPlay scan (setup page picks by index)
static int s_ap_count;

static const char *TAG = "setup";
extern const unsigned char setup_html_start[] asm("_binary_setup_html_start");
extern const unsigned char setup_html_end[] asm("_binary_setup_html_end");

static esp_err_t json_reply(httpd_req_t *r, const char *status, cJSON *body)
{
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!text) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(r, text);
    free(text);
    return err;
}

static esp_err_t error_reply(httpd_req_t *r, const char *status, const char *message)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "message", message);
    return json_reply(r, status, j);
}

static bool api_allowed(httpd_req_t *r)
{
    char header[16], host[96], origin[112], expected[112];
    if (httpd_req_get_hdr_value_str(r, "X-Speaker-Setup", header, sizeof(header)) != ESP_OK ||
        strcmp(header, "1")) return false;
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK) return false;
    // Reject DNS rebinding as well as cross-origin browser requests. No CORS/preflight handler.
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK) return false;
    char address[32], address_port[40];
    snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
    snprintf(address_port, sizeof(address_port), "%s:80", address);
    if (strcmp(host, address) && strcmp(host, address_port)) return false;
    if (httpd_req_get_hdr_value_len(r, "Origin")) {
        if (httpd_req_get_hdr_value_str(r, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
        snprintf(expected, sizeof(expected), "http://%s", host);
        if (strcmp(origin, expected)) return false;
    }
    if (r->method == HTTP_POST) {
        char type[64];
        if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
            (strcmp(type, "application/json") && strncmp(type, "application/json;", 17))) return false;
    }
    return true;
}

static esp_err_t page(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    return httpd_resp_send(r, (const char *)setup_html_start, setup_html_end - setup_html_start - 1);
}

static esp_err_t status(httpd_req_t *r)
{
    if (!api_allowed(r)) return error_reply(r, "403 Forbidden", "Open setup directly on this speaker");
    gls_controller_status_t s;
    gls_controller_get_status(&s);
    cJSON *j = cJSON_CreateObject(), *w = cJSON_AddObjectToObject(j, "wifi");
    wifi_config_t cfg = {0};
    wifi_ap_record_t ap = {0};
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    esp_wifi_sta_get_ap_info(&ap);
    cJSON_AddStringToObject(w, "ssid", (char *)cfg.sta.ssid);
    memset(&cfg, 0, sizeof(cfg));
    cJSON_AddBoolToObject(w, "connected", gls_wifi_is_up());
    cJSON_AddNumberToObject(w, "rssi", ap.rssi);
    cJSON_AddStringToObject(j, "fw_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(j, "state", s.state);
    cJSON_AddStringToObject(j, "remote_state", s.remote_state);
    cJSON_AddBoolToObject(j, "call_active", rtc_peer_active());
    cJSON_AddBoolToObject(j, "call_connected", rtc_peer_connected());
    cJSON_AddNumberToObject(j, "last_hangup_reason", s.last_hangup_reason);
    cJSON_AddNumberToObject(j, "last_hangup_ms", s.last_hangup_ms);
    cJSON_AddStringToObject(j, "last_user_text", s.last_user_text);
    cJSON_AddNumberToObject(j, "last_user_text_ms", s.last_user_text_ms);
    cJSON_AddBoolToObject(j, "has_credentials", s.has_credentials);
    cJSON_AddBoolToObject(j, "logged_in", s.logged_in && s.token_exp > time(NULL));
    cJSON_AddStringToObject(j, "email", s.email);
    cJSON_AddNumberToObject(j, "token_exp", s.token_exp);
    cJSON_AddStringToObject(j, "last_error", s.last_error);
    cJSON_AddBoolToObject(j, "muted", s.muted);
    cJSON_AddNumberToObject(j, "volume", gls_controller_volume());
    cJSON_AddNumberToObject(j, "volume_max", XVF_VOLUME_MAX);
    cJSON_AddNumberToObject(j, "voice_boost", gls_controller_voice_boost());
    cJSON_AddNumberToObject(j, "voice_boost_max", XVF_VOICE_BOOST_MAX_DB);
    cJSON_AddBoolToObject(j, "wake_test", xvf_i2s_wake_test());
    {
        char ap[128]; bool busy = false;
        gls_ap_status(ap, sizeof(ap), &busy);
        cJSON_AddStringToObject(j, "airplay_status", ap);
        cJSON_AddBoolToObject(j, "airplay_busy", busy);
        cJSON_AddNumberToObject(j, "airplay_volume", gls_ap_volume());
        gls_ap_speaker_t out;
        cJSON_AddStringToObject(j, "airplay_output", gls_ap_output(&out) ? out.name : "");
        cJSON_AddBoolToObject(j, "airplay_routed", xvf_i2s_airplay_routed());
        cJSON_AddNumberToObject(j, "airplay_offset_ms", xvf_i2s_airplay_offset_ms());
        char mr[480];
        gls_airplay_measure_result(mr, sizeof(mr));
        cJSON_AddStringToObject(j, "airplay_measure", mr);
        cJSON_AddBoolToObject(j, "airplay_measuring", gls_airplay_measure_running());
        cJSON_AddNumberToObject(j, "airplay_latency_ms", gls_ap_latency_ms());
        cJSON_AddNumberToObject(j, "airplay_measured_ms", gls_ap_measured_delay_ms());
    }
    xvf_eq_t eq;
    gls_controller_eq(&eq);
    cJSON *e = cJSON_AddObjectToObject(j, "eq");
    cJSON_AddBoolToObject(e, "on", eq.on);
    cJSON_AddNumberToObject(e, "hpf_hz", eq.hpf_hz);
    cJSON_AddNumberToObject(e, "warmth_db", eq.warmth_db);
    cJSON_AddNumberToObject(e, "presence_db", eq.presence_db);
    cJSON_AddNumberToObject(e, "treble_db", eq.treble_db);
    cJSON_AddNumberToObject(j, "barge_ins", s.barge_ins);
    cJSON_AddBoolToObject(j, "tunnel_up", gls_wg_is_up());
    cJSON_AddBoolToObject(j, "wake_ready", gls_wake_ready());
    cJSON_AddNumberToObject(j, "wake_detections", gls_wake_detections());
    cJSON_AddNumberToObject(j, "mic_frames", gls_wake_frames());
    cJSON_AddNumberToObject(j, "mic_peak", gls_wake_peak());
    cJSON_AddNumberToObject(j, "wake_score", gls_wake_score());
    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);
    return json_reply(r, "200 OK", j);
}

static esp_err_t diag(httpd_req_t *r)
{
    if (!api_allowed(r)) return error_reply(r, "403 Forbidden", "Invalid setup request");
    gls_diag_levels_t l;
    gls_diag_levels(&l);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "muted", xvf_i2s_mic_muted());
    cJSON_AddNumberToObject(j, "mic_rms_db", (int)(l.mic_rms_db * 10) / 10.0);
    cJSON_AddNumberToObject(j, "mic_peak_db", (int)(l.mic_peak_db * 10) / 10.0);
    cJSON_AddNumberToObject(j, "mic_noise_db", (int)(l.mic_noise_db * 10) / 10.0);
    cJSON_AddNumberToObject(j, "mic_clips", l.mic_clips);
    cJSON_AddNumberToObject(j, "out_rms_db", (int)(l.out_rms_db * 10) / 10.0);
    cJSON_AddNumberToObject(j, "out_peak_db", (int)(l.out_peak_db * 10) / 10.0);
    cJSON_AddNumberToObject(j, "out_limited", l.out_limited);
    cJSON_AddNumberToObject(j, "out_dropouts", l.out_dropouts);
    cJSON_AddBoolToObject(j, "wake_test", xvf_i2s_wake_test());
    cJSON_AddNumberToObject(j, "barge_in_db", -26.9); // 1475/32768 RMS for 120 ms (voice_control.c)
    cJSON_AddNumberToObject(j, "wake_score", gls_wake_score());
    cJSON_AddNumberToObject(j, "wake_cutoff", 72);
    cJSON_AddNumberToObject(j, "wake_detections", gls_wake_detections());
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    gls_diag_wake_event_t ev[GLS_DIAG_EVENTS];
    int n = gls_diag_wake_events(ev, GLS_DIAG_EVENTS);
    cJSON *arr = cJSON_AddArrayToObject(j, "wake_events");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "ago_s", (now - ev[i].t_ms) / 1000);
        cJSON_AddNumberToObject(e, "peak", ev[i].peak);
        cJSON_AddBoolToObject(e, "detected", ev[i].detected);
        cJSON_AddItemToArray(arr, e);
    }
    int pct = 0;
    int st = gls_diag_record_state(&pct);
    cJSON_AddStringToObject(j, "record", st == 1 ? "recording" : st == 2 ? "ready" : "none");
    cJSON_AddNumberToObject(j, "record_pct", pct);
    return json_reply(r, "200 OK", j);
}

static void le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void le32(uint8_t *p, uint32_t v) { le16(p, v); le16(p + 2, v >> 16); }

static esp_err_t recording(httpd_req_t *r)
{
    if (!api_allowed(r)) return error_reply(r, "403 Forbidden", "Invalid setup request");
    size_t n = 0;
    const int16_t *pcm = gls_diag_record_samples(&n);
    if (!pcm) return error_reply(r, "404 Not Found", "No finished recording");
    uint8_t h[44];
    uint32_t bytes = n * 2;
    memcpy(h, "RIFF", 4); le32(h + 4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16); le16(h + 20, 1); le16(h + 22, 1); le32(h + 24, GLS_DIAG_REC_RATE);
    le32(h + 28, GLS_DIAG_REC_RATE * 2); le16(h + 32, 2); le16(h + 34, 16);
    memcpy(h + 36, "data", 4); le32(h + 40, bytes);
    httpd_resp_set_type(r, "audio/wav");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=\"speaker-mic.wav\"");
    if (httpd_resp_send_chunk(r, (const char *)h, sizeof(h)) != ESP_OK) return ESP_FAIL;
    static char chunk[4096]; // internal RAM bounce buffer; the recording lives in PSRAM
    for (size_t off = 0; off < bytes; off += sizeof(chunk)) {
        size_t len = bytes - off < sizeof(chunk) ? bytes - off : sizeof(chunk);
        memcpy(chunk, (const uint8_t *)pcm + off, len);
        if (httpd_resp_send_chunk(r, chunk, len) != ESP_OK) return ESP_FAIL;
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t post(httpd_req_t *r)
{
    if (!api_allowed(r)) return error_reply(r, "403 Forbidden", "Invalid setup request");
    if (r->content_len <= 0 || r->content_len > 24576)
        return error_reply(r, "413 Payload Too Large", "Expected a JSON body under 24 KB");
    char *body = calloc(1, r->content_len + 1);
    if (!body) return error_reply(r, "500 Internal Server Error", "Out of memory");
    int got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, body + got, r->content_len - got);
        if (n <= 0) { memset(body, 0, r->content_len); free(body); return ESP_FAIL; }
        got += n;
    }
    cJSON *j = cJSON_ParseWithLength(body, got + 1);
    memset(body, 0, got); free(body);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsObject(j)) {
        if (!strcmp(r->uri, "/api/setup")) {
            cJSON *session = cJSON_GetObjectItemCaseSensitive(j, "session");
            cJSON *a = cJSON_GetObjectItemCaseSensitive(session, "chunk0");
            cJSON *b = cJSON_GetObjectItemCaseSensitive(session, "chunk1");
            if (cJSON_IsString(a) && a->valuestring[0] && (!b || cJSON_IsNull(b) || cJSON_IsString(b)))
                err = gls_controller_set_credentials(a->valuestring, cJSON_IsString(b) ? b->valuestring : NULL);
            if (cJSON_IsString(a)) memset(a->valuestring, 0, strlen(a->valuestring));
            if (cJSON_IsString(b)) memset(b->valuestring, 0, strlen(b->valuestring));
        } else if (!strcmp(r->uri, "/api/logout")) {
            gls_controller_logout(); err = ESP_OK;
        } else {
            cJSON *action = cJSON_GetObjectItemCaseSensitive(j, "action");
            if (cJSON_IsString(action)) {
                if (!strcmp(action->valuestring, "wake")) { gls_controller_wake(); err = ESP_OK; }
                if (!strcmp(action->valuestring, "hangup")) { gls_controller_hangup(); err = ESP_OK; }
                if (!strcmp(action->valuestring, "mute")) { gls_controller_mute_toggle(); err = ESP_OK; }
                if (!strcmp(action->valuestring, "record")) { gls_diag_record_start(false); err = ESP_OK; }
                if (!strcmp(action->valuestring, "record_out")) { gls_diag_record_start(true); err = ESP_OK; }
                if (!strcmp(action->valuestring, "wake_test")) {
                    cJSON *on = cJSON_GetObjectItemCaseSensitive(j, "on");
                    if (cJSON_IsBool(on)) { xvf_i2s_set_wake_test(cJSON_IsTrue(on)); err = ESP_OK; }
                }
                if (!strcmp(action->valuestring, "airplay_tone") && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "index"))) {
                    int idx = (int)cJSON_GetObjectItemCaseSensitive(j, "index")->valuedouble;
                    if (idx >= 0 && idx < s_ap_count) err = gls_ap_test_tone(&s_ap_found[idx], gls_ap_volume());
                }
                if (!strcmp(action->valuestring, "airplay_output") && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "index"))) {
                    int idx = (int)cJSON_GetObjectItemCaseSensitive(j, "index")->valuedouble;
                    if (idx < 0) { gls_ap_set_output(NULL); err = ESP_OK; }
                    else if (idx < s_ap_count) { gls_ap_set_output(&s_ap_found[idx]); err = ESP_OK; }
                }
                if (!strcmp(action->valuestring, "airplay_offset") && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "value"))) {
                    xvf_i2s_set_airplay_offset_ms((int)cJSON_GetObjectItemCaseSensitive(j, "value")->valuedouble); err = ESP_OK;
                }
                if (!strcmp(action->valuestring, "airplay_measure")) {
                    if (rtc_peer_active()) {
                        cJSON_Delete(j);
                        return error_reply(r, "409 Conflict", "End the conversation first (the measurement needs a quiet speaker)");
                    }
                    err = gls_airplay_measure_start_ex(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "quick")));
                }
                if (!strcmp(action->valuestring, "airplay_volume") && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "value"))) {
                    gls_ap_set_volume((int)cJSON_GetObjectItemCaseSensitive(j, "value")->valuedouble); err = ESP_OK;
                }
                if (!strcmp(action->valuestring, "airplay_pin") && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "pin"))) {
                    gls_ap_submit_pin(cJSON_GetObjectItemCaseSensitive(j, "pin")->valuestring); err = ESP_OK;
                }
                if (!strcmp(action->valuestring, "test_sound")) { gls_chime_play(GLS_CHIME_TEST); err = ESP_OK; }
                cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "value");
                if (!strcmp(action->valuestring, "volume") && cJSON_IsNumber(v))
                    err = gls_controller_set_volume((int)v->valuedouble);
                if (!strcmp(action->valuestring, "eq_reset")) { xvf_eq_t d = XVF_EQ_DEFAULT; err = gls_controller_set_eq(&d); }
                if (!strcmp(action->valuestring, "eq")) {
                    cJSON *on = cJSON_GetObjectItemCaseSensitive(j, "on");
                    cJSON *hp = cJSON_GetObjectItemCaseSensitive(j, "hpf_hz");
                    cJSON *wa = cJSON_GetObjectItemCaseSensitive(j, "warmth_db");
                    cJSON *pr = cJSON_GetObjectItemCaseSensitive(j, "presence_db");
                    cJSON *tr = cJSON_GetObjectItemCaseSensitive(j, "treble_db");
                    if (cJSON_IsBool(on) && cJSON_IsNumber(hp) && cJSON_IsNumber(wa) && cJSON_IsNumber(pr) && cJSON_IsNumber(tr)) {
                        xvf_eq_t e = {.on = cJSON_IsTrue(on), .hpf_hz = (int)hp->valuedouble, .warmth_db = (int)wa->valuedouble,
                                      .presence_db = (int)pr->valuedouble, .treble_db = (int)tr->valuedouble};
                        err = gls_controller_set_eq(&e);
                    }
                }
                if (!strcmp(action->valuestring, "voice_boost") && cJSON_IsNumber(v))
                    err = gls_controller_set_voice_boost((int)v->valuedouble);
            }
        }
    }
    cJSON_Delete(j);
    if (err != ESP_OK) return error_reply(r, "400 Bad Request", "Invalid session or action; nothing saved");
    cJSON *out = cJSON_CreateObject(); cJSON_AddBoolToObject(out, "ok", true);
    return json_reply(r, "200 OK", out);
}

// ---- AirPlay output (experimental): scan for speakers, send a test tone

static esp_err_t airplay_scan(httpd_req_t *r)
{
    if (!api_allowed(r)) return error_reply(r, "403 Forbidden", "Invalid setup request");
    s_ap_count = gls_ap_discover(s_ap_found, (int)(sizeof(s_ap_found) / sizeof(s_ap_found[0])), 1500);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(j, "speakers");
    for (int i = 0; i < s_ap_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", i);
        cJSON_AddStringToObject(o, "name", s_ap_found[i].name);
        cJSON_AddStringToObject(o, "ip", s_ap_found[i].ip);
        cJSON_AddStringToObject(o, "model", s_ap_found[i].model);
        cJSON_AddBoolToObject(o, "airplay2", s_ap_found[i].ap2);
        cJSON_AddBoolToObject(o, "pin", s_ap_found[i].requires_pin);
        cJSON_AddItemToArray(arr, o);
    }
    return json_reply(r, "200 OK", j);
}

// LAN-only XVF3800 tuning/diagnostics:
//   GET /api/xvf?r=17&c=24&n=1&t=f[&v=0.5]   read (and optionally write first) a parameter; t = f|i|b
// (No AEC filter readout: the SPECIAL_CMD filter read sequence wedged the DSP on firmware 1.0.4.)
static int qint(const char *q, const char *k, int def)
{
    char v[16];
    return httpd_query_key_value(q, k, v, sizeof(v)) == ESP_OK ? atoi(v) : def;
}

static esp_err_t xvf_param(httpd_req_t *r)
{
    char q[160] = "";
    httpd_req_get_url_query_str(r, q, sizeof(q));
    cJSON *j = cJSON_CreateObject();
    int res = qint(q, "r", -1), cmd = qint(q, "c", -1), n = qint(q, "n", 1);
    char t[4] = "i", v[96] = "";
    httpd_query_key_value(q, "t", t, sizeof(t));
    int sz = t[0] == 'b' ? 1 : 4;
    if (res < 0 || cmd < 0 || n < 1 || n * sz > 60) {
        cJSON_Delete(j);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "need r, c, n, t");
    }
    uint8_t buf[60];
    if (httpd_query_key_value(q, "v", v, sizeof(v)) == ESP_OK) {
        if (!api_allowed(r)) { cJSON_Delete(j); return error_reply(r, "403 Forbidden", "Invalid setup request"); }
        // URL "," may arrive as %2C
        for (char *p; (p = strstr(v, "%2C")) || (p = strstr(v, "%2c"));) { *p = ','; memmove(p + 1, p + 3, strlen(p + 3) + 1); }
        char *s = v;
        for (int i = 0; i < n; i++) {
            if (t[0] == 'f') { float f = strtof(s, &s); memcpy(buf + 4 * i, &f, 4); }
            else if (t[0] == 'b') buf[i] = (uint8_t)strtol(s, &s, 0);
            else { int32_t x = strtol(s, &s, 0); memcpy(buf + 4 * i, &x, 4); }
            if (*s == ',') s++;
        }
        esp_err_t e = gls_xvf_write(res, cmd, buf, n * sz);
        cJSON_AddStringToObject(j, "write", esp_err_to_name(e));
        ESP_LOGI(TAG, "xvf %d/%d <- %s (%s)", res, cmd, v, esp_err_to_name(e));
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 10 && (e = gls_xvf_read(res, cmd, buf, n * sz)) != ESP_OK; i++) vTaskDelay(pdMS_TO_TICKS(5));
    cJSON_AddStringToObject(j, "read", esp_err_to_name(e));
    cJSON *a = cJSON_AddArrayToObject(j, "value");
    for (int i = 0; e == ESP_OK && i < n; i++) {
        double x;
        if (t[0] == 'f') { float f; memcpy(&f, buf + 4 * i, 4); x = f; }
        else if (t[0] == 'b') x = buf[i];
        else { int32_t y; memcpy(&y, buf + 4 * i, 4); x = y; }
        cJSON_AddItemToArray(a, cJSON_CreateNumber(x));
    }
    return json_reply(r, "200 OK", j);
}

esp_err_t gls_setup_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 4;   // sockets are scarce during calls with AirPlay (see sdkconfig.defaults)
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page},
        {.uri = "/setup", .method = HTTP_GET, .handler = page},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status},
        {.uri = "/api/setup", .method = HTTP_POST, .handler = post},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = post},
        {.uri = "/api/control", .method = HTTP_POST, .handler = post},
        {.uri = "/api/diag", .method = HTTP_GET, .handler = diag},
        {.uri = "/api/diag/record.wav", .method = HTTP_GET, .handler = recording},
        {.uri = "/api/airplay/scan", .method = HTTP_GET, .handler = airplay_scan},
        {.uri = "/api/xvf", .method = HTTP_GET, .handler = xvf_param},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
    ESP_LOGI(TAG, "sign in at http://" IPSTR "/", IP2STR(&ip.ip));
    return ESP_OK;
}
