/* ui.h - the two slow periodic tasks:
 *   control task (every 100 ms): polls the RFID reader and runs the auto-lock / lockout timers
 *   display task (every 200 ms): draws the current state on the OLED */
#pragma once
#include "esp_err.h"

esp_err_t ui_start(void);
