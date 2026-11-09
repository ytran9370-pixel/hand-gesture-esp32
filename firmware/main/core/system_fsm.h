/* system_fsm.h - system lock state machine and gesture -> action mapping (FR-SEC, FR-ACT).
 *
 *   LOCKED  --authorized card-->                 UNLOCKED
 *   LOCKED  --unknown card (3rd time in a row)--> LOCKOUT --30 s--> LOCKED
 *   UNLOCKED --"stop" / authorized card / 30 s idle--> LOCKED
 *
 * Pure logic: no GPIO, no FreeRTOS. Every function returns the reason of the change
 * (FSM_RSN_NONE when nothing changed) so the caller can update outputs and notify the PC. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { LOCK_LOCKED = 0, LOCK_UNLOCKED = 1, LOCK_LOCKOUT = 2 } lock_state_t;

typedef enum {
    ACT_NONE = 0,
    ACT_LED1_TOGGLE = 1,
    ACT_LED2_TOGGLE = 2,
    ACT_LED3_TOGGLE = 3,
    ACT_ALL_ON = 4,
    ACT_ALL_OFF = 5,
    ACT_DOOR_OPEN = 6,
    ACT_DOOR_CLOSE = 7,
    ACT_LOCK = 8,
} action_t;

typedef enum {
    FSM_RSN_NONE = 0,
    FSM_RSN_RFID_OK = 1,
    FSM_RSN_RFID_DENIED = 2,
    FSM_RSN_GESTURE = 3,
    FSM_RSN_AUTO_LOCK_TIMEOUT = 4,
    FSM_RSN_HOST_COMMAND = 5,
    FSM_RSN_LOCKOUT_END = 6,
} fsm_reason_t;

typedef enum { FSM_OK = 0, FSM_ERR_LOCKED, FSM_ERR_ARG } fsm_status_t;

#define OUT_LED1 0x01u
#define OUT_LED2 0x02u
#define OUT_LED3 0x04u
#define OUT_DOOR 0x08u
#define OUT_ALL_LEDS (OUT_LED1 | OUT_LED2 | OUT_LED3)
#define NO_GESTURE 0xFFu

/* Model class indices (order of g_labels in model_data.c). Checked by host/test_logic.c. */
enum {
    CLS_CALL = 0, CLS_DISLIKE = 1, CLS_FIST = 2, CLS_LIKE = 4, CLS_MUTE = 5,
    CLS_ONE = 7, CLS_PALM = 8, CLS_ROCK = 11, CLS_STOP = 12, CLS_COUNT = 18,
};

typedef struct {
    uint32_t unlock_timeout_ms;   /* auto-lock after this long without a confirmed gesture */
    uint32_t lockout_ms;          /* how long all cards are refused after too many denials */
    uint8_t max_denied;           /* denied cards in a row before LOCKOUT */
} fsm_cfg_t;

typedef struct {
    fsm_cfg_t cfg;
    lock_state_t lock;
    uint8_t outputs;
    uint8_t last_gesture;
    uint8_t denied_count;
    uint32_t deadline_ms;         /* auto-lock time (UNLOCKED) or lockout end (LOCKOUT) */
} system_fsm_t;

void fsm_init(system_fsm_t *s, const fsm_cfg_t *cfg);

action_t fsm_gesture_action(int cls);

fsm_reason_t fsm_on_card(system_fsm_t *s, bool authorized, uint32_t now_ms);
fsm_reason_t fsm_on_gesture(system_fsm_t *s, int cls, uint32_t now_ms, action_t *action);
fsm_status_t fsm_set_output(system_fsm_t *s, uint8_t target, uint8_t value, uint32_t now_ms);
fsm_reason_t fsm_tick(system_fsm_t *s, uint32_t now_ms);

uint16_t fsm_remaining_s(const system_fsm_t *s, uint32_t now_ms);
