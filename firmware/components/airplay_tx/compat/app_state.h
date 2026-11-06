// Replacement for AirPlayKnob's app_state.h: only what the sender (raop_AnySpeaker.cpp) touches.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MAX_SPEAKERS 16

struct AirPlaySpeaker {
    char name[48];
    char host[64];
    char ip[20];
    uint16_t port;
    char et[24];
    char cn[24];
    char md[24];
    char am[32];
    bool is_local_dac;
    bool is_ap2;
    bool requires_pin;
    uint32_t last_seen_ms;
};

struct AppState {
    SemaphoreHandle_t mutex;
    AirPlaySpeaker speakers[MAX_SPEAKERS];
    int speaker_count;
    int current_speaker;
    bool playing;
    char status_text[96];
    bool pin_prompt_active;
    bool pin_manual_request;
    bool pin_submitted;
    bool pin_cancelled;
    char pin_code[8];
    char pin_halted_ip[20];
    char pin_speaker_ip[20];
    char pin_speaker_name[48];
};

extern AppState g_app;

void app_notify_pin_toast(const char *title, const char *msg, uint32_t color);
void app_get_ap2_client_identity(char id_out[37], uint8_t ltpk_out[32], uint8_t ltsk_out[64]);
bool app_is_ap2_speaker_paired(const char *ip);
void app_mark_ap2_speaker_paired(const char *ip);
