/* system_fsm.c - see system_fsm.h for the state diagram. */
#include "system_fsm.h"

#include "gesture_filter.h"   /* time_reached() */

void fsm_init(system_fsm_t *s, const fsm_cfg_t *cfg)
{
    s->cfg = *cfg;
    s->lock = LOCK_LOCKED;        /* FR-SEC-01: start LOCKED */
    s->outputs = 0;               /* FR-ACT-03: all off, door closed */
    s->last_gesture = NO_GESTURE;
    s->denied_count = 0;
    s->deadline_ms = 0;
}

/* Table 5.1 of the requirements. Classes that are not listed do nothing (FR-GES-05). */
action_t fsm_gesture_action(int cls)
{
    switch (cls) {
    case CLS_PALM:    return ACT_ALL_ON;
    case CLS_FIST:    return ACT_ALL_OFF;
    case CLS_ONE:     return ACT_LED1_TOGGLE;
    case CLS_ROCK:    return ACT_LED2_TOGGLE;
    case CLS_MUTE:    return ACT_LED3_TOGGLE;
    case CLS_LIKE:    return ACT_DOOR_OPEN;
    case CLS_DISLIKE: return ACT_DOOR_CLOSE;
    case CLS_STOP:    return ACT_LOCK;
    default:          return ACT_NONE;
    }
}

static void unlock(system_fsm_t *s, uint32_t now_ms)
{
    s->lock = LOCK_UNLOCKED;
    s->denied_count = 0;
    s->deadline_ms = now_ms + s->cfg.unlock_timeout_ms;
}

fsm_reason_t fsm_on_card(system_fsm_t *s, bool authorized, uint32_t now_ms)
{
    switch (s->lock) {
    case LOCK_LOCKED:
        if (authorized) {
            unlock(s, now_ms);
            return FSM_RSN_RFID_OK;
        }
        if (++s->denied_count >= s->cfg.max_denied) {
            s->lock = LOCK_LOCKOUT;
            s->deadline_ms = now_ms + s->cfg.lockout_ms;
        }
        return FSM_RSN_RFID_DENIED;
    case LOCK_UNLOCKED:
        if (authorized) {
            s->lock = LOCK_LOCKED;    /* tapping the card again locks (FR-SEC-05) */
            return FSM_RSN_RFID_OK;
        }
        return FSM_RSN_RFID_DENIED;   /* unknown card while unlocked: show "denied", keep state */
    case LOCK_LOCKOUT:
    default:
        return FSM_RSN_NONE;          /* all cards refused (FR-SEC-06) */
    }
}

fsm_reason_t fsm_on_gesture(system_fsm_t *s, int cls, uint32_t now_ms, action_t *action)
{
    *action = ACT_NONE;
    if (s->lock != LOCK_UNLOCKED) {
        return FSM_RSN_NONE;          /* FR-SEC-02: gestures ignored while locked */
    }
    s->last_gesture = (uint8_t)cls;
    s->deadline_ms = now_ms + s->cfg.unlock_timeout_ms;   /* activity restarts the 30 s timer */

    *action = fsm_gesture_action(cls);
    switch (*action) {
    case ACT_LED1_TOGGLE: s->outputs ^= OUT_LED1; break;
    case ACT_LED2_TOGGLE: s->outputs ^= OUT_LED2; break;
    case ACT_LED3_TOGGLE: s->outputs ^= OUT_LED3; break;
    case ACT_ALL_ON:      s->outputs |= OUT_ALL_LEDS; break;
    case ACT_ALL_OFF:     s->outputs &= (uint8_t)~OUT_ALL_LEDS; break;
    case ACT_DOOR_OPEN:   s->outputs |= OUT_DOOR; break;
    case ACT_DOOR_CLOSE:  s->outputs &= (uint8_t)~OUT_DOOR; break;
    case ACT_LOCK:        s->lock = LOCK_LOCKED; break;   /* FR-SEC-07: outputs unchanged */
    case ACT_NONE:
    default:              break;
    }
    return FSM_RSN_GESTURE;
}

fsm_status_t fsm_set_output(system_fsm_t *s, uint8_t target, uint8_t value, uint32_t now_ms)
{
    static const uint8_t k_bits[] = {0, OUT_LED1, OUT_LED2, OUT_LED3, OUT_DOOR};
    if (target < 1 || target > 4 || value > 1) {
        return FSM_ERR_ARG;
    }
    if (s->lock != LOCK_UNLOCKED) {
        return FSM_ERR_LOCKED;
    }
    if (value) {
        s->outputs |= k_bits[target];
    } else {
        s->outputs &= (uint8_t)~k_bits[target];
    }
    s->deadline_ms = now_ms + s->cfg.unlock_timeout_ms;
    return FSM_OK;
}

fsm_reason_t fsm_tick(system_fsm_t *s, uint32_t now_ms)
{
    if (s->lock == LOCK_UNLOCKED && time_reached(now_ms, s->deadline_ms)) {
        s->lock = LOCK_LOCKED;
        return FSM_RSN_AUTO_LOCK_TIMEOUT;
    }
    if (s->lock == LOCK_LOCKOUT && time_reached(now_ms, s->deadline_ms)) {
        s->lock = LOCK_LOCKED;
        s->denied_count = 0;
        return FSM_RSN_LOCKOUT_END;
    }
    return FSM_RSN_NONE;
}

uint16_t fsm_remaining_s(const system_fsm_t *s, uint32_t now_ms)
{
    if (s->lock == LOCK_LOCKED || time_reached(now_ms, s->deadline_ms)) {
        return 0;
    }
    return (uint16_t)((s->deadline_ms - now_ms + 999u) / 1000u);   /* round up: 0.2 s left shows 1 */
}
