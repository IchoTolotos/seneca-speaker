// XVF3800 control over I2C (7-bit 0x2C): write [resid][cmd][n][data...], read via [resid][cmd|0x80][n+1].
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t gls_xvf_init(void);
esp_err_t gls_xvf_write(uint8_t resid, uint8_t cmd, const uint8_t *data, size_t n);
esp_err_t gls_xvf_read(uint8_t resid, uint8_t cmd, uint8_t *data, size_t n);  // payload only (status checked)
// On-board JST speaker amplifier (XVF GPO X0D31, low = enabled).
esp_err_t gls_xvf_amp_enable(bool on);
// AEC reference delay in samples (AUDIO_MGR_SYS_DELAY 35/26, int32); read with gls_xvf_ref_delay().
esp_err_t gls_xvf_set_ref_delay(int32_t samples);
esp_err_t gls_xvf_ref_delay(int32_t *samples);
