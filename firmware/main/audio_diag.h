#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Microphone/playback diagnostics for the setup page: levels of exactly the audio the speaker
// uses (after the XVF3800, before wake word / barge-in / Opus), wake-word near misses, and a
// short recording for listening.

typedef struct {
    float mic_rms_db, mic_peak_db, mic_noise_db; // dBFS over the last 100 ms; noise = slow minimum
    float out_rms_db, out_peak_db;               // what the speaker plays (after volume), last 100 ms
    uint32_t out_limited;                        // samples softened by the limiter since boot
    uint32_t out_dropouts;                       // voice cut off mid-speech (late/lost audio) since boot
    uint32_t mic_clips;                          // samples at full scale since boot
} gls_diag_levels_t;

#define GLS_DIAG_EVENTS 12
typedef struct {
    uint32_t t_ms;   // uptime when the episode peaked
    uint8_t peak;    // highest wake confidence (0-100) in the episode
    bool detected;   // crossed the cutoff
} gls_diag_wake_event_t;

#define GLS_DIAG_REC_SECONDS 5
#define GLS_DIAG_REC_RATE 16000

void gls_diag_init(void);
void gls_diag_mic(const int16_t *pcm, size_t n);  // from the microphone task (10 ms blocks)
void gls_diag_out(const int16_t *pcm, size_t n, uint32_t limited); // from the I2S writer: exactly what plays
void gls_diag_dropout(void);                       // from the I2S writer: speech cut off by a gap
void gls_diag_wake_score(int score, bool detected); // from the wake-word task, every inference

void gls_diag_levels(gls_diag_levels_t *out);
// Copies up to max events, newest first; returns the count.
int gls_diag_wake_events(gls_diag_wake_event_t *out, int max);

// Recording: start arms a fresh 5 s capture; state 0 = none, 1 = recording, 2 = ready.
// Source: microphone, or the speaker output (starts with the next played audio).
void gls_diag_record_start(bool speaker_output);
int gls_diag_record_state(int *progress_pct);
// Pointer to the finished 16-bit mono samples (only valid while state == 2).
const int16_t *gls_diag_record_samples(size_t *count);

#ifdef __cplusplus
}
#endif
