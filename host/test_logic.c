/* test_logic.c - unit tests for the hardware-independent firmware modules (firmware/main/core).
 * Runs on the PC and in CI: make -C host */
#include <stdio.h>
#include <string.h>

#include "crc16.h"
#include "gesture_filter.h"
#include "model.h"
#include "proto.h"
#include "system_fsm.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failed++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/* ---------------------------------------------------------------- CRC / protocol */
static uint8_t g_buf[PROTO_MAX_PAYLOAD];

static parse_result_t feed_all(proto_parser_t *p, const uint8_t *bytes, size_t n)
{
    parse_result_t r = PARSE_NONE;
    for (size_t i = 0; i < n; i++) {
        r = proto_parser_feed(p, bytes[i]);
        if (r != PARSE_NONE && i + 1 != n) {
            return r;   /* result before the last byte: unexpected for these tests */
        }
    }
    return r;
}

static void test_crc(void)
{
    puts("crc16");
    CHECK(crc16_ccitt((const uint8_t *)"123456789", 9, CRC16_INIT) == 0x29B1);
    /* computing in two parts gives the same result */
    uint16_t c = crc16_ccitt((const uint8_t *)"1234", 4, CRC16_INIT);
    CHECK(crc16_ccitt((const uint8_t *)"56789", 5, c) == 0x29B1);
}

static void test_example_frames(void)
{
    puts("protocol example frames (HG-ICD-001 section 6)");
    uint8_t hdr[PROTO_HEADER_SIZE];

    /* PING, seq 1 = A5 5A 01 01 00 00 44 C5 */
    uint16_t crc = proto_build_header(hdr, MSG_PING, 1, NULL, 0);
    const uint8_t ping[] = {0xA5, 0x5A, 0x01, 0x01, 0x00, 0x00};
    CHECK(memcmp(hdr, ping, 6) == 0);
    CHECK(crc == 0xC544);

    /* GET_STATE, seq 2 -> CRC 39 CD */
    CHECK(proto_build_header(hdr, MSG_GET_STATE, 2, NULL, 0) == 0xCD39);

    /* SET_OUTPUT LED2 ON, seq 3 -> CRC A8 B3 */
    const req_set_output_t req = {.target = 2, .value = 1};
    CHECK(proto_build_header(hdr, MSG_SET_OUTPUT, 3, &req, sizeof(req)) == 0xB3A8);

    /* ERROR(CRC) for request 0x01, seq 7 -> CRC 23 27 */
    const rsp_error_t err = {.code = PROTO_ERR_CRC, .req_type = MSG_PING};
    CHECK(proto_build_header(hdr, MSG_ERROR, 7, &err, sizeof(err)) == 0x2723);
}

static void test_parser(void)
{
    puts("frame parser");
    proto_parser_t p;
    proto_parser_init(&p, g_buf, sizeof(g_buf));

    const uint8_t ping[] = {0xA5, 0x5A, 0x01, 0x01, 0x00, 0x00, 0x44, 0xC5};
    CHECK(feed_all(&p, ping, sizeof(ping)) == PARSE_FRAME);
    CHECK(p.type == MSG_PING && p.seq == 1 && p.len == 0);
    CHECK(!proto_parser_busy(&p));

    /* garbage and a false start before the frame are skipped (resynchronisation, FR-COM-03) */
    const uint8_t noisy[] = {0x00, 0xFF, 0xA5, 0x13, 0xA5, 0xA5, 0x5A, 0x07, 0x03, 0x02, 0x00, 0x02, 0x01, 0xA8, 0xB3};
    CHECK(feed_all(&p, noisy, sizeof(noisy)) == PARSE_FRAME);
    CHECK(p.type == MSG_SET_OUTPUT && p.seq == 3 && p.len == 2 && g_buf[0] == 2 && g_buf[1] == 1);

    /* one flipped bit -> CRC error, and the next frame is still received */
    uint8_t bad[sizeof(ping)];
    memcpy(bad, ping, sizeof(ping));
    bad[3] ^= 0x04;
    CHECK(feed_all(&p, bad, sizeof(bad)) == PARSE_CRC_ERROR);
    CHECK(feed_all(&p, ping, sizeof(ping)) == PARSE_FRAME);

    /* LEN larger than the buffer is rejected right after the LEN field */
    const uint8_t too_long[] = {0xA5, 0x5A, 0x02, 0x09, 0xFF, 0xFF};
    CHECK(feed_all(&p, too_long, sizeof(too_long)) == PARSE_LEN_ERROR);
    CHECK(!proto_parser_busy(&p));

    /* busy flag is set inside a frame and cleared by reset (used for the 50 ms timeout) */
    proto_parser_feed(&p, 0xA5);
    proto_parser_feed(&p, 0x5A);
    CHECK(proto_parser_busy(&p));
    proto_parser_reset(&p);
    CHECK(!proto_parser_busy(&p));

    /* a full-size image frame, built with proto_build_header, round-trips */
    static uint8_t img[PROTO_IMAGE_BYTES];
    for (size_t i = 0; i < sizeof(img); i++) {
        img[i] = (uint8_t)(i * 7u);
    }
    uint8_t hdr[PROTO_HEADER_SIZE];
    uint16_t crc = proto_build_header(hdr, MSG_INFER, 42, img, sizeof(img));
    parse_result_t r = feed_all(&p, hdr, sizeof(hdr));
    CHECK(r == PARSE_NONE);
    for (size_t i = 0; i < sizeof(img); i++) {
        proto_parser_feed(&p, img[i]);
    }
    proto_parser_feed(&p, (uint8_t)(crc & 0xFF));
    CHECK(proto_parser_feed(&p, (uint8_t)(crc >> 8)) == PARSE_FRAME);
    CHECK(p.type == MSG_INFER && p.seq == 42 && p.len == sizeof(img) && memcmp(g_buf, img, sizeof(img)) == 0);
}

/* ---------------------------------------------------------------- gesture filter */
static void test_gesture_filter(void)
{
    puts("gesture filter (FR-GES-03, FR-GES-04)");
    const gesture_filter_cfg_t cfg = {.min_count = 3, .min_conf_permille = 800, .cooldown_ms = 1500};
    gesture_filter_t f;
    gf_init(&f, &cfg);

    /* 3 confident images of the same class -> confirmed on the 3rd */
    CHECK(gf_feed(&f, CLS_LIKE, 900, 0) == -1 && gf_count(&f) == 1);
    CHECK(gf_feed(&f, CLS_LIKE, 900, 280) == -1 && gf_count(&f) == 2);
    CHECK(gf_feed(&f, CLS_LIKE, 900, 560) == CLS_LIKE);

    /* cooldown: everything ignored for 1.5 s */
    CHECK(gf_feed(&f, CLS_LIKE, 990, 840) == -1);
    CHECK(gf_feed(&f, CLS_LIKE, 990, 1500) == -1);
    CHECK(gf_feed(&f, CLS_LIKE, 990, 2059) == -1);
    /* cooldown ends at 560 + 1500 = 2060: this image starts a new sequence */
    CHECK(gf_feed(&f, CLS_LIKE, 990, 2060) == -1 && gf_count(&f) == 1);

    /* a low-confidence image breaks the sequence */
    gf_init(&f, &cfg);
    gf_feed(&f, CLS_PALM, 900, 0);
    gf_feed(&f, CLS_PALM, 900, 100);
    CHECK(gf_feed(&f, CLS_PALM, 799, 200) == -1 && gf_count(&f) == 0);
    CHECK(gf_feed(&f, CLS_PALM, 800, 300) == -1 && gf_count(&f) == 1);   /* 0.800 is accepted */

    /* a different class restarts counting at 1 */
    gf_init(&f, &cfg);
    gf_feed(&f, CLS_PALM, 900, 0);
    gf_feed(&f, CLS_PALM, 900, 100);
    CHECK(gf_feed(&f, CLS_FIST, 900, 200) == -1 && gf_count(&f) == 1);
    gf_feed(&f, CLS_FIST, 900, 300);
    CHECK(gf_feed(&f, CLS_FIST, 900, 400) == CLS_FIST);

    /* millisecond counter wrap-around (after ~49 days) does not break the cooldown */
    gf_init(&f, &cfg);
    const uint32_t t = 0xFFFFFF00u;
    gf_feed(&f, CLS_ONE, 900, t);
    gf_feed(&f, CLS_ONE, 900, t + 10);
    CHECK(gf_feed(&f, CLS_ONE, 900, t + 20) == CLS_ONE);
    CHECK(gf_feed(&f, CLS_ONE, 900, t + 1000) == -1);    /* still cooling down after the wrap */
    CHECK(gf_feed(&f, CLS_ONE, 900, t + 1520) == -1 && gf_count(&f) == 1);
}

/* ---------------------------------------------------------------- system state machine */
static const fsm_cfg_t k_fsm_cfg = {.unlock_timeout_ms = 30000, .lockout_ms = 30000, .max_denied = 3};

static void test_mapping(void)
{
    puts("gesture -> action mapping (Table 5.1) and class indices");
    /* class index constants must match the model's label order */
    CHECK(strcmp(g_labels[CLS_CALL], "call") == 0);
    CHECK(strcmp(g_labels[CLS_DISLIKE], "dislike") == 0);
    CHECK(strcmp(g_labels[CLS_FIST], "fist") == 0);
    CHECK(strcmp(g_labels[CLS_LIKE], "like") == 0);
    CHECK(strcmp(g_labels[CLS_MUTE], "mute") == 0);
    CHECK(strcmp(g_labels[CLS_ONE], "one") == 0);
    CHECK(strcmp(g_labels[CLS_PALM], "palm") == 0);
    CHECK(strcmp(g_labels[CLS_ROCK], "rock") == 0);
    CHECK(strcmp(g_labels[CLS_STOP], "stop") == 0);
    CHECK(CLS_COUNT == NUM_CLASSES);

    CHECK(fsm_gesture_action(CLS_PALM) == ACT_ALL_ON);
    CHECK(fsm_gesture_action(CLS_FIST) == ACT_ALL_OFF);
    CHECK(fsm_gesture_action(CLS_ONE) == ACT_LED1_TOGGLE);
    CHECK(fsm_gesture_action(CLS_ROCK) == ACT_LED2_TOGGLE);
    CHECK(fsm_gesture_action(CLS_MUTE) == ACT_LED3_TOGGLE);
    CHECK(fsm_gesture_action(CLS_LIKE) == ACT_DOOR_OPEN);
    CHECK(fsm_gesture_action(CLS_DISLIKE) == ACT_DOOR_CLOSE);
    CHECK(fsm_gesture_action(CLS_STOP) == ACT_LOCK);
    /* exactly 8 classes have an action; the other 10 do nothing (FR-GES-05) */
    int n = 0;
    for (int c = 0; c < NUM_CLASSES; c++) {
        n += fsm_gesture_action(c) != ACT_NONE;
    }
    CHECK(n == 8);
}

static void test_fsm(void)
{
    puts("system state machine (FR-SEC)");
    system_fsm_t s;
    action_t a;
    fsm_init(&s, &k_fsm_cfg);

    /* FR-SEC-01, FR-ACT-03: start LOCKED with everything off */
    CHECK(s.lock == LOCK_LOCKED && s.outputs == 0 && s.last_gesture == NO_GESTURE);

    /* FR-SEC-02: gestures and host commands ignored while LOCKED */
    CHECK(fsm_on_gesture(&s, CLS_PALM, 0, &a) == FSM_RSN_NONE && a == ACT_NONE && s.outputs == 0);
    CHECK(fsm_set_output(&s, 1, 1, 0) == FSM_ERR_LOCKED);

    /* FR-SEC-03: authorized card unlocks */
    CHECK(fsm_on_card(&s, true, 1000) == FSM_RSN_RFID_OK && s.lock == LOCK_UNLOCKED);
    CHECK(fsm_remaining_s(&s, 1000) == 30);

    /* actions */
    CHECK(fsm_on_gesture(&s, CLS_PALM, 2000, &a) == FSM_RSN_GESTURE && a == ACT_ALL_ON && s.outputs == OUT_ALL_LEDS);
    fsm_on_gesture(&s, CLS_ONE, 3000, &a);
    CHECK(s.outputs == (OUT_LED2 | OUT_LED3));
    fsm_on_gesture(&s, CLS_ONE, 4000, &a);
    CHECK(s.outputs == OUT_ALL_LEDS);
    fsm_on_gesture(&s, CLS_LIKE, 5000, &a);
    CHECK(a == ACT_DOOR_OPEN && (s.outputs & OUT_DOOR));
    fsm_on_gesture(&s, CLS_FIST, 6000, &a);
    CHECK(s.outputs == OUT_DOOR);   /* lights off, door still open */
    CHECK(s.last_gesture == CLS_FIST);

    /* unmapped class: no output change, but it is activity and restarts the timer */
    CHECK(fsm_on_gesture(&s, CLS_CALL, 7000, &a) == FSM_RSN_GESTURE && a == ACT_NONE && s.outputs == OUT_DOOR);

    /* host command */
    CHECK(fsm_set_output(&s, 3, 1, 8000) == FSM_OK && (s.outputs & OUT_LED3));
    CHECK(fsm_set_output(&s, 5, 1, 8000) == FSM_ERR_ARG);
    CHECK(fsm_set_output(&s, 1, 2, 8000) == FSM_ERR_ARG);

    /* FR-SEC-05 + FR-SEC-07: "stop" locks, outputs keep their state */
    const uint8_t before = s.outputs;
    CHECK(fsm_on_gesture(&s, CLS_STOP, 9000, &a) == FSM_RSN_GESTURE && a == ACT_LOCK);
    CHECK(s.lock == LOCK_LOCKED && s.outputs == before && fsm_remaining_s(&s, 9000) == 0);

    /* FR-SEC-05: authorized card while UNLOCKED locks again */
    fsm_on_card(&s, true, 10000);
    CHECK(fsm_on_card(&s, true, 11000) == FSM_RSN_RFID_OK && s.lock == LOCK_LOCKED);

    /* FR-SEC-05: auto-lock 30 s after the last activity */
    fsm_on_card(&s, true, 20000);
    fsm_on_gesture(&s, CLS_ROCK, 25000, &a);                 /* restarts timer: deadline 55000 */
    CHECK(fsm_tick(&s, 54999) == FSM_RSN_NONE && s.lock == LOCK_UNLOCKED);
    CHECK(fsm_remaining_s(&s, 54500) == 1);
    CHECK(fsm_tick(&s, 55000) == FSM_RSN_AUTO_LOCK_TIMEOUT && s.lock == LOCK_LOCKED);
    CHECK(fsm_tick(&s, 56000) == FSM_RSN_NONE);
}

static void test_lockout(void)
{
    puts("lockout after 3 denied cards (FR-SEC-04, FR-SEC-06)");
    system_fsm_t s;
    fsm_init(&s, &k_fsm_cfg);

    CHECK(fsm_on_card(&s, false, 0) == FSM_RSN_RFID_DENIED && s.lock == LOCK_LOCKED && s.denied_count == 1);
    CHECK(fsm_on_card(&s, false, 100) == FSM_RSN_RFID_DENIED && s.denied_count == 2);
    CHECK(fsm_on_card(&s, false, 200) == FSM_RSN_RFID_DENIED && s.lock == LOCK_LOCKOUT);

    /* during LOCKOUT even an authorized card is refused */
    CHECK(fsm_on_card(&s, true, 1000) == FSM_RSN_NONE && s.lock == LOCK_LOCKOUT);
    CHECK(fsm_tick(&s, 30199) == FSM_RSN_NONE);
    CHECK(fsm_tick(&s, 30200) == FSM_RSN_LOCKOUT_END && s.lock == LOCK_LOCKED && s.denied_count == 0);
    CHECK(fsm_on_card(&s, true, 31000) == FSM_RSN_RFID_OK && s.lock == LOCK_UNLOCKED);

    /* a good card resets the denied counter */
    fsm_init(&s, &k_fsm_cfg);
    fsm_on_card(&s, false, 0);
    fsm_on_card(&s, false, 0);
    fsm_on_card(&s, true, 0);
    CHECK(s.denied_count == 0);
    fsm_on_card(&s, true, 0);       /* lock again */
    fsm_on_card(&s, false, 0);
    CHECK(s.lock == LOCK_LOCKED && s.denied_count == 1);
}

int main(void)
{
    test_crc();
    test_example_frames();
    test_parser();
    test_gesture_filter();
    test_mapping();
    test_fsm();
    test_lockout();
    printf("%d checks, %d failed -> %s\n", g_checks, g_failed, g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
