#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GLS_CHIME_CONNECTED,    // two notes up: call is live
    GLS_CHIME_DISCONNECTED, // same two notes down: call ended
    GLS_CHIME_WAKE_TEST,    // single short high beep: "Seneca" heard in wake-test mode
    GLS_CHIME_TEST,         // 2 s steady tone (setup page): crackle on a pure tone = hardware, not network
} gls_chime_t;

// Queue a short UI sound; returns immediately (played by a low-priority task).
void gls_chime_play(gls_chime_t which);

#ifdef __cplusplus
}
#endif
