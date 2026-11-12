/* oled.h - minimal SSD1306 driver (I2C, 128x64 or 128x32) with a 5x7 text font.
 *
 * Drawing happens in a RAM frame buffer; oled_flush() sends it to the display in one I2C write.
 * 21 characters per line, 8 lines (128x64) or 4 lines (128x32). */
#pragma once
#include <stdbool.h>

#include "esp_err.h"

#define OLED_WIDTH 128
#define OLED_COLS (OLED_WIDTH / 6)   /* 5 px glyph + 1 px gap */

/* Returns an error (and the rest of the firmware continues) if no display answers (FR-DSP-03). */
esp_err_t oled_init(void);
bool oled_ready(void);
int oled_lines(void);

void oled_clear(void);
/* Text on line `line` (0 = top). `invert` draws white background with black text. */
void oled_text(int line, const char *text, bool invert);
esp_err_t oled_flush(void);
