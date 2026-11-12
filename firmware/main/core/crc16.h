/* crc16.h - CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR).
 * Check value: crc16_ccitt("123456789", 9, CRC16_INIT) == 0x29B1. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CRC16_INIT 0xFFFFu

/* Pass the previous result as `crc` to compute the CRC over several buffers. */
uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc);
