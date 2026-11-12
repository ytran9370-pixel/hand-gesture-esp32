/* font5x7.h - 5x7 ASCII font, characters 0x20 (space) to 0x7E (~). */
#pragma once
#include <stdint.h>

#define FONT5X7_W 5
#define FONT5X7_FIRST 0x20
#define FONT5X7_COUNT 95

extern const uint8_t g_font5x7[FONT5X7_COUNT][FONT5X7_W];
