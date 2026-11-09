/* oled.c - SSD1306 over I2C (ESP-IDF i2c_master driver).
 *
 * Every I2C write starts with a control byte: 0x00 = the following bytes are commands,
 * 0x40 = the following bytes are display data. The display memory is organised in "pages":
 * one page = 8 pixel rows, one byte = one column of 8 pixels (bit 0 at the top). */
#include "oled.h"

#include <string.h>

#include "app_config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "font5x7.h"

#define OLED_HEIGHT CONFIG_HG_OLED_HEIGHT
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_I2C_HZ 400000
#define OLED_TIMEOUT_MS 50

static const char *TAG = "oled";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

/* byte 0 = control byte 0x40, then the frame buffer: one transfer per flush */
static uint8_t s_tx[1 + OLED_WIDTH * OLED_PAGES];
static uint8_t *const s_fb = &s_tx[1];

static esp_err_t send_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[32];
    buf[0] = 0x00;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(s_dev, buf, n + 1, OLED_TIMEOUT_MS);
}

esp_err_t oled_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* most OLED modules also have 4.7k pull-ups */
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus");

    /* SSD1306 modules use address 0x3C, a few use 0x3D */
    uint16_t addr = 0;
    if (i2c_master_probe(s_bus, 0x3C, OLED_TIMEOUT_MS) == ESP_OK) {
        addr = 0x3C;
    } else if (i2c_master_probe(s_bus, 0x3D, OLED_TIMEOUT_MS) == ESP_OK) {
        addr = 0x3D;
    } else {
        ESP_LOGW(TAG, "no SSD1306 at 0x3C/0x3D - check SDA=%d SCL=%d, VCC, GND", PIN_I2C_SDA, PIN_I2C_SCL);
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = OLED_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "I2C device");

    const uint8_t init[] = {
        0xAE,                                   /* display off */
        0xD5, 0x80,                             /* clock divider */
        0xA8, OLED_HEIGHT - 1,                  /* multiplex ratio = number of rows - 1 */
        0xD3, 0x00,                             /* display offset 0 */
        0x40,                                   /* start line 0 */
        0x8D, 0x14,                             /* enable the internal charge pump */
        0x20, 0x00,                             /* horizontal addressing mode */
        0xA1,                                   /* mirror columns (module mounted this way) */
        0xC8,                                   /* scan rows from bottom to top */
        0xDA, OLED_HEIGHT == 64 ? 0x12 : 0x02,  /* COM pin layout differs for 64 and 32 rows */
        0x81, 0x8F,                             /* contrast */
        0xD9, 0xF1,                             /* pre-charge period */
        0xDB, 0x40,                             /* VCOMH level */
        0xA4,                                   /* show RAM content */
        0xA6,                                   /* normal (not inverted) */
        0xAF,                                   /* display on */
    };
    ESP_RETURN_ON_ERROR(send_cmds(init, sizeof(init)), TAG, "init sequence");

    s_tx[0] = 0x40;
    s_ready = true;
    oled_clear();
    ESP_LOGI(TAG, "SSD1306 128x%d at 0x%02X", OLED_HEIGHT, addr);
    return oled_flush();
}

bool oled_ready(void)
{
    return s_ready;
}

int oled_lines(void)
{
    return OLED_PAGES;
}

void oled_clear(void)
{
    memset(s_fb, 0, OLED_WIDTH * OLED_PAGES);
}

void oled_text(int line, const char *text, bool invert)
{
    if (line < 0 || line >= OLED_PAGES) {
        return;
    }
    uint8_t *row = &s_fb[line * OLED_WIDTH];
    memset(row, invert ? 0xFF : 0x00, OLED_WIDTH);
    for (int col = 0; col < OLED_COLS && text[col] != '\0'; col++) {
        unsigned char c = (unsigned char)text[col];
        if (c < FONT5X7_FIRST || c >= FONT5X7_FIRST + FONT5X7_COUNT) {
            c = '?';
        }
        for (int x = 0; x < FONT5X7_W; x++) {
            const uint8_t bits = g_font5x7[c - FONT5X7_FIRST][x];
            row[col * 6 + x] = invert ? (uint8_t)~bits : bits;
        }
    }
}

esp_err_t oled_flush(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t window[] = {0x21, 0, OLED_WIDTH - 1, 0x22, 0, OLED_PAGES - 1};   /* write whole screen */
    ESP_RETURN_ON_ERROR(send_cmds(window, sizeof(window)), TAG, "window");
    return i2c_master_transmit(s_dev, s_tx, sizeof(s_tx), OLED_TIMEOUT_MS);
}
