/* ui.c - control task (RFID + timers) and display task. */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model.h"
#include "oled.h"
#include "rc522.h"

/* A card counts as removed after this many polls in a row without an answer.
 * One missed poll is normal (RF noise), so a single miss must not re-trigger the card. */
#define CARD_GONE_POLLS 3

/* ---------------------------------------------------------------- control task */
static void control_task(void *arg)
{
    (void)arg;
    uint8_t uid[RC522_UID_LEN];
    uint8_t current[RC522_UID_LEN] = {0};
    bool present = false;
    int misses = 0;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        if (rc522_ready()) {
            if (rc522_read_uid(uid)) {
                misses = 0;
                /* trigger once per tap: only when a card arrives (or a different card replaces it) */
                if (!present || memcmp(uid, current, sizeof(uid)) != 0) {
                    present = true;
                    memcpy(current, uid, sizeof(uid));
                    app_state_on_card(uid);
                }
            } else if (present && ++misses >= CARD_GONE_POLLS) {
                present = false;
            }
        }
        app_state_tick();
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));   /* fixed period, no drift */
    }
}

/* ---------------------------------------------------------------- display task */
static const char *gesture_name(int cls)
{
    return (cls >= 0 && cls < NUM_CLASSES) ? g_labels[cls] : "-";
}

static void draw(const app_view_t *v)
{
    static const char *const lock_names[] = {"LOCKED", "UNLOCKED", "LOCKOUT"};
    const sys_state_t *s = &v->state;
    const uint8_t o = s->outputs;
    char line[48];   /* oled_text() cuts at OLED_COLS characters */

    oled_clear();

    /* line 0: lock state + countdown, inverted so it stands out */
    if (v->show_denied) {
        snprintf(line, sizeof(line), "ACCESS DENIED");
    } else if (s->lock_state == 1 || s->lock_state == 2) {
        snprintf(line, sizeof(line), "%s %us", lock_names[s->lock_state], (unsigned)s->unlock_remaining_s);
    } else {
        snprintf(line, sizeof(line), "%s", s->lock_state < 3 ? lock_names[s->lock_state] : "?");
    }
    oled_text(0, line, true);

    /* line 1: lights */
    snprintf(line, sizeof(line), "L1:%s L2:%s L3:%s", (o & 0x01) ? "ON" : "--", (o & 0x02) ? "ON" : "--",
             (o & 0x04) ? "ON" : "--");
    oled_text(1, line, false);

    /* line 2: door, or the UID of a refused card (so it can be added to the whitelist) */
    if (v->show_denied) {
        snprintf(line, sizeof(line), "UID %02X%02X%02X%02X", v->denied_uid[0], v->denied_uid[1], v->denied_uid[2],
                 v->denied_uid[3]);
    } else {
        snprintf(line, sizeof(line), "Door:%s", (o & 0x08) ? "OPEN" : "CLOSED");
    }
    oled_text(2, line, false);

    /* line 3: what the model sees right now */
    if (v->pred_cls >= 0) {
        snprintf(line, sizeof(line), "%.11s %3u%% x%u", gesture_name(v->pred_cls), v->pred_conf_permille / 10u,
                 v->filter_count);
    } else {
        snprintf(line, sizeof(line), "waiting for PC...");
    }
    oled_text(3, line, false);

    /* 128x64 only: extra lines */
    if (oled_lines() >= 8) {
        snprintf(line, sizeof(line), "Last: %s", s->last_gesture == 0xFF ? "-" : gesture_name(s->last_gesture));
        oled_text(5, line, false);
        snprintf(line, sizeof(line), "RFID:%s  fw %s", v->rfid_ok ? "OK" : "NONE", FW_VERSION);
        oled_text(7, line, false);
    }
    oled_flush();
}

static void display_task(void *arg)
{
    (void)arg;
    app_view_t v;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        app_state_view(&v);   /* copy under the lock, draw without it */
        draw(&v);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(DISPLAY_PERIOD_MS));
    }
}

esp_err_t ui_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(control_task, "control", TASK_CONTROL_STACK, NULL, TASK_CONTROL_PRIO, NULL,
                                            TASK_CONTROL_CORE);
    if (ok == pdPASS && oled_ready()) {
        ok = xTaskCreatePinnedToCore(display_task, "display", TASK_DISPLAY_STACK, NULL, TASK_DISPLAY_PRIO, NULL,
                                     TASK_DISPLAY_CORE);
    }
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
