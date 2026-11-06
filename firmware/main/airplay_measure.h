// Acoustic AirPlay latency / flush measurement (uses the speaker's own microphones).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t gls_airplay_measure_start(void);   // needs an AirPlay output speaker; not during a call
// quick: only the current latency setting (~15 s) instead of trying 2000/1000/500/250 ms
esp_err_t gls_airplay_measure_start_ex(bool quick);
bool gls_airplay_measure_running(void);
void gls_airplay_measure_result(char *out, size_t len);
