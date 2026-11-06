// Full-duplex I2S to the XVF3800 (firmware i2s_slave_v1.0.9_16k): ESP32 is I2S master,
// 16 kHz, 32-bit slots, stereo Philips, TX+RX on one controller sharing BCLK/WS.
// Both channels are enabled once at boot and never stopped: the XVF3800 needs a steady clock
// and the AEC needs a continuous far-end reference (TX auto-clears to silence on underrun).
//
// See docs/hardware-bringup.md section 4 and main/board_xvf3800.h.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t xvf_i2s_init(void);

/**
 * Read `samples` mono 16-bit samples (16 kHz) from the XVF3800's processed output
 * (slot CONFIG_GLS_MIC_SLOT, top 16 bits of the 32-bit sample). Blocks for the audio.
 * Returns zeros while muted (the read still runs, keeping timing).
 */
esp_err_t xvf_i2s_read_mono16(int16_t *out, size_t samples, uint32_t timeout_ms);

/**
 * Write mono 16-bit samples (16 kHz) to both slots as 32-bit, after digital volume.
 * GPIO44 feeds the AIC3104 DAC and the XVF3800 AEC reference at once.
 */
esp_err_t xvf_i2s_write_mono16(const int16_t *in, size_t samples, uint32_t timeout_ms);

/**
 * Play a short UI sound (<= 0.5 s, 16 kHz mono) mixed into the output: over the downlink stream
 * if one is playing, otherwise on its own. Blocks until it has been handed to I2S.
 */
esp_err_t xvf_i2s_play_overlay(const int16_t *pcm, size_t samples);

/** Enable the voice-capture feed; wake detection always receives microphone audio. */
void xvf_i2s_set_capture_enabled(bool enabled);

void xvf_i2s_set_mic_mute(bool mute);
bool xvf_i2s_mic_muted(void);

/** Digital playback volume 0..XVF_VOLUME_MAX (applied before I2S so the AEC reference matches what plays).
 * Above 100 the gain exceeds 1 and peaks are soft-limited. */
#define XVF_VOLUME_MAX 150
void xvf_i2s_set_volume(int percent);
/** Extra gain for the ChatGPT voice only (not chimes/test sound), 0..XVF_VOICE_BOOST_MAX_DB dB. */
#define XVF_VOICE_BOOST_MAX_DB 12
void xvf_i2s_set_voice_boost(int db);
/** Speaker EQ, applied to everything played (voice, chimes) before the volume; the AEC reference
 * gets the same signal. Defaults suit a small passive speaker without an enclosure. */
typedef struct {
    bool on;
    int hpf_hz;      /* bass cut (high-pass, 12 dB/oct); 0 = off, 40..300 */
    int warmth_db;   /* peak at 400 Hz, -6..+9 */
    int presence_db; /* peak at 2.5 kHz, -6..+6 */
    int treble_db;   /* high shelf from 5 kHz, -12..+6 */
} xvf_eq_t;
#define XVF_EQ_DEFAULT ((xvf_eq_t){.on = true, .hpf_hz = 150, .warmth_db = 3, .presence_db = 0, .treble_db = -3})
void xvf_i2s_set_eq(const xvf_eq_t *eq);
bool xvf_eq_valid(const xvf_eq_t *eq);

/** AirPlay output routing (active while an AirPlay stream is ready, see airplay_tx). */
void xvf_i2s_airplay_flush(void);                 /* barge-in: drop queued AirPlay + delayed reference */
void xvf_i2s_set_airplay_offset_ms(int ms);        /* trim of the AEC reference delay */
int xvf_i2s_airplay_offset_ms(void);
bool xvf_i2s_airplay_routed(void);

/** Measurement helpers (AirPlay latency/flush measurement). */
void xvf_i2s_set_mic_tap(void (*fn)(const int16_t *pcm, size_t n));
void xvf_i2s_set_ref_mute(bool on);
void xvf_i2s_overlay_start(const int16_t *pcm, size_t samples);
void xvf_i2s_overlay_stop(void);

/** Wake-word test: while on, the detector hears the mic even when muted (the call never does). */
void xvf_i2s_set_wake_test(bool on);
bool xvf_i2s_wake_test(void);

#ifdef __cplusplus
}
#endif
