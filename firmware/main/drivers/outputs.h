/* outputs.h - 3 LEDs (GPIO) and the door servo (LEDC PWM, 50 Hz). */
#pragma once
#include <stdint.h>

#include "esp_err.h"

/* Configures the pins; all LEDs off and door closed (FR-ACT-03). */
esp_err_t outputs_init(void);

/* bits: OUT_LED1..OUT_LED3, OUT_DOOR (system_fsm.h). The servo only moves when the door bit changes. */
void outputs_apply(uint8_t bits);
