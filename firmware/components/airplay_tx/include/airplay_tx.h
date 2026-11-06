// AirPlay sender (AirPlay 1 RAOP + AirPlay 2 with HomeKit transient/PIN pairing) to e.g. a HomePod mini.
// Built on the MIT-licensed AirPlayKnob sender (vendor/). C API for the firmware.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[48];
    char ip[20];
    uint16_t port;
    char model[32];
    bool ap2;
    bool requires_pin;
} gls_ap_speaker_t;

// Starts mDNS (once). Safe to call repeatedly.
esp_err_t gls_ap_init(void);
// Blocking scan (_raop._tcp + _airplay._tcp), returns the number of speakers found.
int gls_ap_discover(gls_ap_speaker_t *out, int max, int timeout_ms);
// Starts an async 2 s, 440 Hz test tone to the given speaker. ESP_ERR_INVALID_STATE if busy.
esp_err_t gls_ap_test_tone(const gls_ap_speaker_t *spk, int volume_pct);
// Human-readable state of the last/ongoing AirPlay action; busy = a task is running.
void gls_ap_status(char *out, size_t len, bool *busy);
// AirPlay volume 0..100 (0 = mute, 1..100 -> -30..0 dB on the receiver), persisted; applied live if connected.
void gls_ap_set_volume(int pct);
int gls_ap_volume(void);
// ---- live streaming (call audio -> AirPlay speaker)
// Remember which speaker calls should play on (persisted). NULL clears it (built-in speaker).
void gls_ap_set_output(const gls_ap_speaker_t *spk);
bool gls_ap_output(gls_ap_speaker_t *out);           // true if an AirPlay output is configured
// Connect (async, ~2 s) and start streaming silence; push 16 kHz mono PCM with gls_ap_stream_push.
esp_err_t gls_ap_stream_start(void);
void gls_ap_stream_stop(void);
bool gls_ap_stream_ready(void);                      // connected and sending
int gls_ap_stream_delay_ms(void);                    // push -> receiver playback (buffer + AirPlay latency)
void gls_ap_stream_push(const int16_t *mono16k, size_t samples);
void gls_ap_stream_flush(void);
// Exact AEC reference: the 16 kHz samples the receiver plays starting at esp_timer time t_us
// (from the RTP-indexed send history and the measured receiver delay). false if not available.
bool gls_ap_ref_read(int64_t t_us, int16_t *out, size_t n);
void gls_ap_set_play_delay_ms(int ms);              // measured RTP time -> sound (NVS)
int gls_ap_play_delay_ms(void);
void gls_ap_use_play_delay_ms(int ms);              // same, runtime only (safe from PSRAM-stack tasks)
void gls_ap_onset_arm(void);                         // mark the next loud sent frame...
int64_t gls_ap_onset_us(void);                       // ...its RTP time as esp_timer us (-1 = none yet)
int gls_ap_stream_queue_samples(void);               // 16 kHz samples waiting in the sender's queue                      // barge-in: drop queued + receiver-buffered audio
// Requested receiver latency in ms (0 = default 2000), persisted; and the acoustically measured
// push->sound delay (0 = unknown) used to align the AEC reference.
void gls_ap_set_latency_ms(int ms);
int gls_ap_latency_ms(void);
void gls_ap_set_measured_delay_ms(int ms);
int gls_ap_measured_delay_ms(void);
// PIN pairing (only if the speaker demands it; HomePods normally use transient pairing).
void gls_ap_submit_pin(const char *pin);

#ifdef __cplusplus
}
#endif
