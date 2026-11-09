/* app_config.h - every pin, timing and task setting in one place.
 * Values that change between boards or users (card UIDs, OLED size) are in menuconfig
 * ("Hand Gesture configuration", see Kconfig.projbuild) or sdkconfig.defaults. */
#pragma once
#include "sdkconfig.h"

#define FW_VERSION "1.0.0"
#define MODEL_NAME "hand_gesture_int8_qat"

/* ---------------- pins (ESP32-S3-N16R8, TDMaker TDM2533) ----------------
 * Avoid GPIO 19/20 (native USB), 26-32 (flash) and 33-37 (octal PSRAM). */
#define PIN_UART_TX 43          /* UART0 -> on-board USB-UART bridge ("COM" connector) */
#define PIN_UART_RX 44
#define PIN_I2C_SDA 8           /* SSD1306 OLED */
#define PIN_I2C_SCL 9
#define PIN_SPI_SCK 12          /* RC522 RFID reader */
#define PIN_SPI_MOSI 11
#define PIN_SPI_MISO 13
#define PIN_RC522_CS 10
#define PIN_RC522_RST 14
#define PIN_LED1 4
#define PIN_LED2 5
#define PIN_LED3 6
#define PIN_SERVO 7

/* ---------------- communication ---------------- */
#define LINK_UART_NUM 0
#define LINK_BAUD CONFIG_HG_UART_BAUD
#define LINK_RX_TIMEOUT_MS 50   /* inter-byte gap that aborts a frame (FR-COM-04) */
#define LINK_JOB_BUFFERS 2      /* receive image N+1 while image N is inferred (NFR-PERF-04) */

/* ---------------- behaviour (requirements HG-SRS-001) ---------------- */
#define GESTURE_MIN_COUNT 3             /* FR-GES-03 */
#define GESTURE_MIN_CONF_PERMILLE 800   /* FR-GES-03 */
#define GESTURE_COOLDOWN_MS 1500        /* FR-GES-04 */
#define UNLOCK_TIMEOUT_MS 30000         /* FR-SEC-05 */
#define LOCKOUT_MS 30000                /* FR-SEC-06 */
#define MAX_DENIED_CARDS 3              /* FR-SEC-06 */
#define DENIED_MESSAGE_MS 2000          /* FR-SEC-04 */
#define SERVO_CLOSED_DEG 0              /* FR-ACT-02 */
#define SERVO_OPEN_DEG 90

/* ---------------- FreeRTOS tasks ----------------
 * Core 1 does only inference. Everything that talks to hardware runs on core 0.
 * Higher number = higher priority. Receiving bytes must never wait for inference. */
#define TASK_LINK_RX_PRIO 12
#define TASK_LINK_RX_CORE 0
#define TASK_LINK_RX_STACK 4096

#define TASK_INFER_PRIO 10
#define TASK_INFER_CORE 1
#define TASK_INFER_STACK 6144

#define TASK_CONTROL_PRIO 6             /* RFID polling + auto-lock timer */
#define TASK_CONTROL_CORE 0
#define TASK_CONTROL_STACK 4096
#define CONTROL_PERIOD_MS 100

#define TASK_DISPLAY_PRIO 2             /* lowest: the display must never delay inference (FR-DSP-02) */
#define TASK_DISPLAY_CORE 0
#define TASK_DISPLAY_STACK 4096
#define DISPLAY_PERIOD_MS 200
