// Controller: the one task that owns speaker_core's session FSM and wm_events tracker.
//
//   inputs (queue) ──► sc_fsm_handle ──► actions
//     button / auto-call (WAKE, HANGUP, MUTE)        START_AUTH / START_CALL / HANG_UP / CLEAR
//     Wi-Fi up/down                                     └──► worker task (HTTP + esp_peer), results
//     worker results (AUTH_*, SIGNAL_*)                      come back as inputs with the same seq
//     peer events (PEER_CONNECTED/FAILED/CLOSED)     STOP_SPEAKING / MUTE / LED: done in place
//     data-channel messages -> sc_wm_decode -> REMOTE_STATE / REMOTE_CLOSE / USAGE / SPAWN / ACTIVITY
//     ~1 s TICK; remote audio packets also count as "remote speaking"
#pragma once
#include "xvf3800_i2s.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the tasks. Loads the session; posts WIFI_UP if Wi-Fi is already connected. */
esp_err_t gls_controller_start(void);

/** Wi-Fi edge (safe to call before start: ignored). */
void gls_controller_wifi(bool up);

/** Setup page / console stored new credentials (ownership of strings stays with caller). */
esp_err_t gls_controller_set_credentials(const char *chunk0, const char *chunk1);

typedef struct {
    char state[24];
    char remote_state[24];
    char last_user_text[160];
    uint64_t last_user_text_ms, last_hangup_ms;
    int last_hangup_reason;
    char email[128];
    char last_error[96];
    int64_t token_exp;
    uint32_t barge_ins;
    bool has_credentials;
    bool logged_in;
    bool muted;
} gls_controller_status_t;
void gls_controller_get_status(gls_controller_status_t *out);
void gls_controller_wake(void);
/** Called by the continuous microphone reader; no I/O or allocation. */
void gls_controller_microphone(const int16_t *pcm, size_t count);
void gls_controller_hangup(void);
void gls_controller_mute_toggle(void);
// Playback volume 0..XVF_VOLUME_MAX percent, persisted in NVS.
int gls_controller_volume(void);
esp_err_t gls_controller_set_volume(int percent);
// Extra gain for the ChatGPT voice only, 0..XVF_VOICE_BOOST_MAX_DB dB, persisted in NVS.
int gls_controller_voice_boost(void);
esp_err_t gls_controller_set_voice_boost(int db);
// Speaker EQ (see xvf_eq_t), persisted in NVS.
void gls_controller_eq(xvf_eq_t *out);
esp_err_t gls_controller_set_eq(const xvf_eq_t *eq);
void gls_controller_logout(void);

#ifdef __cplusplus
}
#endif
