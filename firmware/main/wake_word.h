#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t gls_wake_start(void);
void gls_wake_feed(const int16_t *samples, size_t count);
bool gls_wake_ready(void);
uint32_t gls_wake_detections(void);
uint32_t gls_wake_frames(void);
int gls_wake_peak(void);
int gls_wake_score(void);
#ifdef __cplusplus
}
#endif
