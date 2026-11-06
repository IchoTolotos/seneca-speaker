// XIAO user LED (GPIO21) showing speaker_core's sc_led_t with blink patterns.
#pragma once
#include "session_fsm.h"

void gls_led_init(void);
void gls_led_set(sc_led_t mode);
