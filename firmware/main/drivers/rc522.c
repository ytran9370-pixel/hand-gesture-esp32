/* rc522.c - MFRC522 driver (register level, SPI mode 0).
 *
 * SPI address byte: bit 7 = 1 for read / 0 for write, bits 6..1 = register, bit 0 = 0.
 * Reading a card UID uses three ISO 14443A commands:
 *   1. WUPA (0x52, 7 bits)     wake up every card in the field (also cards in HALT state)
 *   2. ANTICOLL (0x93 0x20)    the card answers with UID0..UID3 + BCC (XOR check byte)
 *   3. HLTA (0x50 0x00 + CRC)  put the card back to sleep so the next poll starts cleanly
 * The chip's internal timer aborts a command after 25 ms when no card answers. */
#include "rc522.h"

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* registers */
#define REG_COMMAND 0x01
#define REG_COM_IRQ 0x04
#define REG_ERROR 0x06
#define REG_FIFO_DATA 0x09
#define REG_FIFO_LEVEL 0x0A
#define REG_CONTROL 0x0C
#define REG_BIT_FRAMING 0x0D
#define REG_COLL 0x0E
#define REG_MODE 0x11
#define REG_TX_CONTROL 0x14
#define REG_TX_ASK 0x15
#define REG_T_MODE 0x2A
#define REG_T_PRESCALER 0x2B
#define REG_T_RELOAD_H 0x2C
#define REG_T_RELOAD_L 0x2D
#define REG_VERSION 0x37

/* chip commands */
#define CMD_IDLE 0x00
#define CMD_TRANSCEIVE 0x0C
#define CMD_SOFT_RESET 0x0F

/* card commands */
#define PICC_WUPA 0x52
#define PICC_ANTICOLL_CL1 0x93
#define PICC_HLTA 0x50

#define RC522_SPI_HZ (5 * 1000 * 1000)

static const char *TAG = "rc522";
static spi_device_handle_t s_spi;
static bool s_ready;

static void wr(uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {
        .length = 16,
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = {(uint8_t)((reg << 1) & 0x7E), val},
    };
    spi_device_polling_transmit(s_spi, &t);
}

static uint8_t rd(uint8_t reg)
{
    spi_transaction_t t = {
        .length = 16,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = {(uint8_t)(((reg << 1) & 0x7E) | 0x80), 0},
    };
    spi_device_polling_transmit(s_spi, &t);
    return t.rx_data[1];
}

static void set_bits(uint8_t reg, uint8_t mask)
{
    wr(reg, rd(reg) | mask);
}

static void clear_bits(uint8_t reg, uint8_t mask)
{
    wr(reg, rd(reg) & (uint8_t)~mask);
}

/* ISO 14443A CRC (CRC_A): init 0x6363, used by HLTA. */
static uint16_t crc_a(const uint8_t *data, int n)
{
    uint16_t crc = 0x6363;
    for (int i = 0; i < n; i++) {
        uint8_t b = data[i] ^ (uint8_t)(crc & 0xFF);
        b ^= (uint8_t)(b << 4);
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ (b >> 4));
    }
    return crc;
}

/* Send `n_tx` bytes to the card and read the answer. `last_bits` = bits of the last byte to send
 * (0 = all 8). Returns the number of bytes received, or -1 on timeout/error. */
static int transceive(const uint8_t *tx, int n_tx, uint8_t last_bits, uint8_t *rx, int rx_cap)
{
    wr(REG_COMMAND, CMD_IDLE);
    wr(REG_COM_IRQ, 0x7F);           /* clear all interrupt flags */
    wr(REG_FIFO_LEVEL, 0x80);        /* flush FIFO */
    for (int i = 0; i < n_tx; i++) {
        wr(REG_FIFO_DATA, tx[i]);
    }
    wr(REG_BIT_FRAMING, last_bits);
    wr(REG_COMMAND, CMD_TRANSCEIVE);
    set_bits(REG_BIT_FRAMING, 0x80); /* StartSend */

    /* wait for RxIRq/IdleIRq (answer received) or TimerIRq (25 ms, no card).
     * 1 tick = 1 ms (CONFIG_FREERTOS_HZ=1000), so 40 tries cover the 25 ms timer. */
    uint8_t irq = 0;
    for (int i = 0; i < 40; i++) {
        irq = rd(REG_COM_IRQ);
        if (irq & 0x31) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    clear_bits(REG_BIT_FRAMING, 0x80);

    if (!(irq & 0x30)) {
        return -1;                   /* timeout: no card answered */
    }
    if (rd(REG_ERROR) & 0x13) {      /* buffer overflow, parity or protocol error */
        return -1;
    }
    int n = rd(REG_FIFO_LEVEL);
    if (n > rx_cap) {
        n = rx_cap;
    }
    for (int i = 0; i < n; i++) {
        rx[i] = rd(REG_FIFO_DATA);
    }
    return n;
}

esp_err_t rc522_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED), TAG, "SPI bus");

    const spi_device_interface_config_t dev = {
        .clock_speed_hz = RC522_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_RC522_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "SPI device");

    /* hardware reset: RST low, then high */
    gpio_reset_pin(PIN_RC522_RST);
    gpio_set_direction(PIN_RC522_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RC522_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_RC522_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    wr(REG_COMMAND, CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));

    const uint8_t version = rd(REG_VERSION);
    if (version == 0x00 || version == 0xFF) {
        ESP_LOGW(TAG, "no answer (version 0x%02X) - check SCK=%d MOSI=%d MISO=%d CS=%d RST=%d, 3.3 V", version,
                 PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO, PIN_RC522_CS, PIN_RC522_RST);
        return ESP_ERR_NOT_FOUND;
    }

    wr(REG_T_MODE, 0x80);            /* timer starts automatically after each transmission */
    wr(REG_T_PRESCALER, 0xA9);       /* 13.56 MHz / (2 * 0xA9 + 1) ~ 40 kHz -> 25 us per tick */
    wr(REG_T_RELOAD_H, 0x03);        /* 1000 ticks = 25 ms timeout */
    wr(REG_T_RELOAD_L, 0xE8);
    wr(REG_TX_ASK, 0x40);            /* 100 % ASK modulation */
    wr(REG_MODE, 0x3D);              /* CRC preset 0x6363 */
    set_bits(REG_TX_CONTROL, 0x03);  /* antenna on */

    s_ready = true;
    ESP_LOGI(TAG, "MFRC522 version 0x%02X", version);
    return ESP_OK;
}

bool rc522_ready(void)
{
    return s_ready;
}

bool rc522_read_uid(uint8_t uid[RC522_UID_LEN])
{
    if (!s_ready) {
        return false;
    }
    uint8_t rx[5];

    const uint8_t wupa = PICC_WUPA;
    if (transceive(&wupa, 1, 7, rx, sizeof(rx)) != 2) {   /* short frame (7 bits); answer = 2-byte ATQA */
        return false;
    }

    clear_bits(REG_COLL, 0x80);
    const uint8_t anticoll[] = {PICC_ANTICOLL_CL1, 0x20};
    if (transceive(anticoll, 2, 0, rx, sizeof(rx)) != 5) {
        return false;
    }
    if ((rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) {       /* BCC check */
        return false;
    }
    for (int i = 0; i < RC522_UID_LEN; i++) {
        uid[i] = rx[i];
    }

    uint8_t hlta[4] = {PICC_HLTA, 0x00, 0, 0};
    const uint16_t crc = crc_a(hlta, 2);
    hlta[2] = (uint8_t)(crc & 0xFF);
    hlta[3] = (uint8_t)(crc >> 8);
    transceive(hlta, 4, 0, rx, sizeof(rx));                /* no answer expected */
    return true;
}
