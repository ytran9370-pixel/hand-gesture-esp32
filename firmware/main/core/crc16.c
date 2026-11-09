/* crc16.c - bitwise CRC-16/CCITT-FALSE. 12 KB per image takes well under 1 ms at 240 MHz,
 * so a 512-byte lookup table is not needed. */
#include "crc16.h"

uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
