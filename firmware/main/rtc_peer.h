// One WebRTC call to ChatGPT /realtime/wm, driven with esp_peer directly
// (esp_webrtc is not on the component registry).
//
// Offer: audio only (Opus, sendrecv), SCTP data channel, host candidates, no STUN/TURN
// (the server is ICE-lite with public host candidates; we are the controlling agent).
// See tools/fake-speaker/README.md "SDP facts" and firmware/README.md "WebRTC choices".
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTC_PEER_EV_CONNECTED,    /*!< ICE + DTLS up (media flows) */
    RTC_PEER_EV_DC_OPEN,      /*!< data channel usable */
    RTC_PEER_EV_DISCONNECTED, /*!< connection lost after being up */
    RTC_PEER_EV_REMOTE_CLOSED, /*!< server closed the call/control channel; do not redial */
    RTC_PEER_EV_FAILED,       /*!< could not connect */
} rtc_peer_event_t;

typedef struct {
    /** Local offer ready; `sdp` is malloc'd and owned by the callee. Called from the peer task. */
    void (*on_offer)(char *sdp, void *ctx);
    void (*on_event)(rtc_peer_event_t ev, void *ctx);
    /** One data-channel message (not NUL-terminated). Called from the peer task: copy it. */
    void (*on_data)(const uint8_t *data, int len, void *ctx);
    void *ctx;
} rtc_peer_callbacks_t;

/** Open a peer and start creating the offer (delivered via on_offer). */
esp_err_t rtc_peer_start(const rtc_peer_callbacks_t *cb);

/** Apply the SDP answer from /realtime/wm. */
esp_err_t rtc_peer_apply_answer(const char *sdp);

/** Send a text message on the data channel (stream id 0). */
esp_err_t rtc_peer_send_text(const char *text);

/** Tear down (idempotent). Must not be called from rtc_peer callbacks. */
void rtc_peer_stop(void);

bool rtc_peer_active(void);
bool rtc_peer_connected(void);

/** esp_timer ms of the last received remote audio packet (0 = none this call). */
uint64_t rtc_peer_last_remote_audio_ms(void);

/** Remote audio packets received in this call. */
uint32_t rtc_peer_remote_audio_packets(void);

#ifdef __cplusplus
}
#endif
