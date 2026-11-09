/* gesture_filter.h - debounce for model predictions (FR-GES-03, FR-GES-04).
 *
 * A gesture is CONFIRMED only after `min_count` consecutive images of the same class, each with
 * confidence >= `min_conf_permille`. After a confirmation all images are ignored for `cooldown_ms`,
 * so holding one gesture does not fire the same action again and again.
 *
 *   IDLE --(conf ok)--> CANDIDATE --(count == min_count)--> COOLDOWN --(time)--> IDLE
 *
 * Time is passed in by the caller (milliseconds), so the filter can be tested on a PC. */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t min_count;
    uint16_t min_conf_permille;
    uint32_t cooldown_ms;
} gesture_filter_cfg_t;

typedef enum { GF_IDLE, GF_CANDIDATE, GF_COOLDOWN } gf_state_t;

typedef struct {
    gesture_filter_cfg_t cfg;
    gf_state_t state;
    int cls;
    uint8_t count;
    uint32_t cooldown_end_ms;
} gesture_filter_t;

void gf_init(gesture_filter_t *f, const gesture_filter_cfg_t *cfg);

/* Feed one prediction. Returns the confirmed class, or -1 if nothing is confirmed by this image. */
int gf_feed(gesture_filter_t *f, int cls, uint16_t conf_permille, uint32_t now_ms);

/* Number of consecutive matching images so far (reported to the PC as filter_count). */
uint8_t gf_count(const gesture_filter_t *f);

/* Wrap-safe "now >= deadline" for 32-bit millisecond counters. */
static inline int time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}
