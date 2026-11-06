// Media system: XVF3800 I2S <-> Opus.
//   uplink:   xvf3800_i2s (mono 16 kHz) -> custom esp_capture audio source -> Opus encoder (esp_capture)
//   downlink: Opus frames -> av_render (decoder + FIFO + resample to 16 kHz mono) -> custom render -> I2S
// No software AEC anywhere: the XVF3800 does it with the reference wired from GPIO44.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Registers Opus only, sets thread schedulers, starts I2S, opens capture + player. Call once. */
esp_err_t gls_media_init(void);

/** Uplink: create the Opus sink and start capturing. */
esp_err_t gls_media_capture_start(void);
void gls_media_capture_stop(void);

typedef void (*gls_media_frame_cb_t)(const uint8_t *data, int size, uint32_t pts, void *ctx);
/** Hand every encoded Opus frame that is ready to `cb` (non-blocking). Returns frames sent. */
int gls_media_capture_drain(gls_media_frame_cb_t cb, void *ctx);

/** Downlink: (re)declare the incoming stream (from esp_peer's on_audio_info). */
esp_err_t gls_media_play_stream(uint32_t sample_rate, uint8_t channels);
/** Queue one Opus packet for decoding/playback. */
void gls_media_play_opus(const uint8_t *data, int size, uint32_t pts);
/** Drop queued audio now (stop_speaking / barge-in ducking). */
void gls_media_play_flush(void);
// Jitter-buffer underruns since boot (speech ran dry mid-utterance).
uint32_t gls_media_underruns(void);
// Current jitter-buffer target (ms of speech buffered before playback starts).
uint32_t gls_media_buffer_ms(void);
// 10 ms quiet slices inserted / skipped to keep the buffer near its target.
uint32_t gls_media_stretched(void);
uint32_t gls_media_dropped(void);
/** Briefly silence late downlink packets after interruption, without stopping uplink. */
void gls_media_play_suppress(uint32_t duration_ms);
/** End of call: reset the player. */
void gls_media_play_reset(void);

#ifdef __cplusplus
}
#endif
