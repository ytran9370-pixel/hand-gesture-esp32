/* gesture_filter.c - see gesture_filter.h for the state diagram. */
#include "gesture_filter.h"

void gf_init(gesture_filter_t *f, const gesture_filter_cfg_t *cfg)
{
    f->cfg = *cfg;
    f->state = GF_IDLE;
    f->cls = -1;
    f->count = 0;
    f->cooldown_end_ms = 0;
}

int gf_feed(gesture_filter_t *f, int cls, uint16_t conf_permille, uint32_t now_ms)
{
    if (f->state == GF_COOLDOWN) {
        if (!time_reached(now_ms, f->cooldown_end_ms)) {
            return -1;              /* still cooling down: ignore this image */
        }
        f->state = GF_IDLE;         /* cooldown over: handle this image normally */
        f->count = 0;
    }

    if (conf_permille < f->cfg.min_conf_permille) {
        f->state = GF_IDLE;         /* unsure prediction breaks the sequence */
        f->count = 0;
        return -1;
    }

    if (f->state == GF_CANDIDATE && cls == f->cls) {
        f->count++;
    } else {
        f->state = GF_CANDIDATE;    /* first image of a new candidate */
        f->cls = cls;
        f->count = 1;
    }

    if (f->count >= f->cfg.min_count) {
        f->state = GF_COOLDOWN;
        f->cooldown_end_ms = now_ms + f->cfg.cooldown_ms;
        return cls;
    }
    return -1;
}

uint8_t gf_count(const gesture_filter_t *f)
{
    return f->count;
}
