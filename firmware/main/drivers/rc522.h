/* rc522.h - MFRC522 13.56 MHz RFID reader over SPI: reads the UID of ISO 14443A cards
 * (MIFARE Classic cards and key fobs sold with the RC522 module). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define RC522_UID_LEN 4

/* Returns ESP_ERR_NOT_FOUND if the chip does not answer (the firmware then runs without RFID). */
esp_err_t rc522_init(void);
bool rc522_ready(void);

/* Polls once. Returns true and fills uid if a card is in the field right now. */
bool rc522_read_uid(uint8_t uid[RC522_UID_LEN]);
