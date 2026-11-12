/* main.c - Hand Gesture firmware for the ESP32-S3.
 *
 * Boot order: outputs to a safe state first, then the self-test, then the optional peripherals,
 * and the UART link last, so the PC never talks to a half-initialised device.
 *
 *   core 0: link_rx (prio 12)  control (6)  display (2)
 *   core 1: infer (10)
 *
 * Logs go to the native USB port (USB-Serial-JTAG); UART0 carries only protocol frames. */
#include <stdbool.h>

#include "app_config.h"
#include "app_state.h"
#include "commands.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "link.h"
#include "model.h"
#include "oled.h"
#include "outputs.h"
#include "rc522.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Hand Gesture firmware %s", FW_VERSION);

    /* 1. outputs first: LEDs off, door closed (FR-ACT-03) */
    ESP_ERROR_CHECK(outputs_init());

    /* 2. optional peripherals: a missing OLED or RFID reader must not stop the system (FR-DSP-03) */
    const bool oled_ok = oled_init() == ESP_OK;
    if (oled_ok) {
        oled_text(0, "HAND GESTURE", true);
        oled_text(2, "self-test...", false);
        oled_flush();
    }
#if CONFIG_HG_ENABLE_RFID
    const bool rfid_ok = rc522_init() == ESP_OK;
#else
    const bool rfid_ok = false;
#endif

    /* 3. self-test with the golden images (FR-DIAG-01) */
    const int passed = commands_post();
    if (passed != g_num_test) {
        ESP_LOGE(TAG, "self-test FAILED - inference results are not trustworthy");
    }

    /* 4. state, tasks, and finally the PC link */
    app_state_init(rfid_ok);
    ESP_ERROR_CHECK(link_init());
    ESP_ERROR_CHECK(commands_start());
    ESP_ERROR_CHECK(ui_start());
    ESP_ERROR_CHECK(link_start());

    ESP_LOGI(TAG, "ready: OLED %s, RFID %s, free internal RAM %u bytes", oled_ok ? "ok" : "missing",
             rfid_ok ? "ok" : "missing", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    /* app_main returns; its task is deleted and the four tasks above keep running */
}
