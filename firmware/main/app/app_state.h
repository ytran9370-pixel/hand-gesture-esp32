/* app_state.h - the one place that owns the system state.
 *
 * Wraps the pure state machine (core/system_fsm) with a mutex, drives the outputs after every change
 * and notifies the PC with EVT_STATE_CHANGED (FR-COM-05). Called from the infer task (gestures,
 * host commands), the control task (RFID, timer) and the display task (read only). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "proto.h"
#include "system_fsm.h"

/* Everything the display needs, copied in one go so the display never holds the lock while drawing. */
typedef struct {
    sys_state_t state;
    int pred_cls;               /* last model prediction, -1 = none yet */
    uint16_t pred_conf_permille;
    uint8_t filter_count;
    bool show_denied;           /* "ACCESS DENIED" message is active (FR-SEC-04) */
    uint8_t denied_uid[4];
    bool rfid_ok;
} app_view_t;

void app_state_init(bool rfid_ok);

void app_state_on_card(const uint8_t uid[4]);
/* Returns the action executed (ACT_NONE if locked or the class has no action). */
action_t app_state_on_gesture(int cls);
fsm_status_t app_state_set_output(uint8_t target, uint8_t value);
void app_state_tick(void);

void app_state_set_prediction(int cls, uint16_t conf_permille, uint8_t filter_count);
void app_state_get(sys_state_t *out);
void app_state_view(app_view_t *out);
