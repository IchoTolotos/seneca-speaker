// Local interruption timing and addressed hang-up phrase recognition.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t above_since_ms;
    uint64_t last_interrupt_ms;
    bool above;
    bool interrupted;
} sc_barge_in_t;

/** PCM16 input: 0.045 RMS for >=120ms while assistant speaks, >=800ms cooldown.
 * Muted/inactive audio resets the hold. Does not start calls. One audio-thread owner.
 */
bool sc_barge_in_feed(sc_barge_in_t *state, const int16_t *pcm, size_t count,
                      uint64_t now_ms, bool assistant_speaking, bool muted);

/** Match a complete stop/call-ending command, never a substring inside normal speech. */
bool sc_voice_is_hangup(const char *text);

typedef struct {
    uint64_t prefix_until_ms, confirm_at_ms;
    bool addressed, candidate;
} sc_voice_stop_t;
/** Whole or fragmented user transcript; confirm after 500ms without further words. */
void sc_voice_stop_update(sc_voice_stop_t *state, const char *text, uint64_t now_ms);
bool sc_voice_stop_poll(sc_voice_stop_t *state, uint64_t now_ms);
#ifdef __cplusplus
}
#endif
