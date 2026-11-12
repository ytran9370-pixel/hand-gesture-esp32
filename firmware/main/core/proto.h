/* proto.h - UART frame protocol v2 shared by the firmware and the PC.
 *
 *   A5 5A | TYPE | SEQ | LEN (u16 LE) | PAYLOAD (LEN bytes) | CRC-16 (u16 LE)
 *
 * The CRC covers TYPE, SEQ, LEN and PAYLOAD. All payload structures are packed and little-endian.
 * This file has no ESP-IDF dependency so it is unit tested on the PC (host/test_logic.c). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTO_VERSION 2
#define PROTO_SOF0 0xA5
#define PROTO_SOF1 0x5A
#define PROTO_HEADER_SIZE 6
#define PROTO_CRC_SIZE 2
#define PROTO_IMAGE_BYTES (64 * 64 * 3)
#define PROTO_MAX_PAYLOAD (PROTO_IMAGE_BYTES + 64)

typedef enum {
    MSG_PING = 0x01,
    MSG_INFER = 0x02,
    MSG_SELFTEST = 0x03,
    MSG_STATS = 0x04,
    MSG_PROFILE = 0x05,
    MSG_GET_STATE = 0x06,
    MSG_SET_OUTPUT = 0x07,
    MSG_EVT_STATE_CHANGED = 0x41,
    MSG_ERROR = 0x7F,
} msg_type_t;

#define MSG_RESPONSE(type) ((uint8_t)((type) | 0x80))

typedef enum {
    PROTO_ERR_CRC = 1,
    PROTO_ERR_LEN = 2,
    PROTO_ERR_UNKNOWN_CMD = 3,
    PROTO_ERR_BUSY = 4,
    PROTO_ERR_TIMEOUT = 5,
    PROTO_ERR_INTERNAL = 6,
    PROTO_ERR_LOCKED = 7,
} proto_error_t;

/* ---------------- payload structures ---------------- */
#define PACKED __attribute__((packed))

typedef struct PACKED {
    uint8_t proto_version;
    uint8_t n_classes;
    uint16_t in_w;
    uint16_t in_h;
    uint8_t in_c;
    uint8_t n_layers;
    uint32_t model_hash;
    uint32_t macs;
    uint32_t cpu_mhz;
    char backend[8];
    char model_name[32];
    char fw_version[16];
} rsp_ping_t;

typedef struct PACKED {
    uint8_t class_id;
    uint8_t n_classes;
    uint16_t conf_permille;
    uint32_t infer_us;
    uint32_t device_us;
    uint8_t action;
    uint8_t filter_count;
    uint8_t lock_state;
    uint8_t outputs;
    /* followed by int32_t logits[n_classes] */
} rsp_infer_head_t;

typedef struct PACKED {
    uint8_t passed;
    uint8_t total;
    uint16_t reserved;
    uint32_t avg_us;
    uint32_t min_us;
    uint32_t max_us;
} rsp_selftest_t;

typedef struct PACKED {
    uint32_t uptime_ms;
    uint32_t n_frames_ok;
    uint32_t n_infer;
    uint32_t n_crc_err;
    uint32_t n_len_err;
    uint32_t n_timeout;
    uint32_t heap_internal_free;
    uint32_t heap_internal_min;
    uint32_t psram_free;
    uint32_t last_infer_us;
    uint8_t post_passed;
    uint8_t post_total;
    uint16_t reserved;
} rsp_stats_t;

typedef struct PACKED {
    char name[12];
    uint32_t time_us;
} rsp_profile_entry_t;

typedef struct PACKED {
    uint8_t lock_state;
    uint8_t outputs;            /* bit 0-2 = LED1-LED3, bit 3 = door open */
    uint8_t last_gesture;       /* 0xFF = none */
    uint8_t denied_count;
    uint16_t unlock_remaining_s;
} sys_state_t;

typedef struct PACKED {
    uint8_t target;             /* 1-3 = LED1-LED3, 4 = door */
    uint8_t value;              /* 0 = off/closed, 1 = on/open */
} req_set_output_t;

typedef struct PACKED {
    sys_state_t state;
    uint8_t reason;
} evt_state_changed_t;

typedef struct PACKED {
    uint8_t code;
    uint8_t req_type;
} rsp_error_t;

/* Sizes are part of the protocol contract (HG-ICD-001 section 5.1): fail the build if they change. */
_Static_assert(sizeof(rsp_ping_t) == 76, "rsp_ping_t size");
_Static_assert(sizeof(rsp_infer_head_t) == 16, "rsp_infer_head_t size");
_Static_assert(sizeof(rsp_selftest_t) == 16, "rsp_selftest_t size");
_Static_assert(sizeof(rsp_stats_t) == 44, "rsp_stats_t size");
_Static_assert(sizeof(rsp_profile_entry_t) == 16, "rsp_profile_entry_t size");
_Static_assert(sizeof(sys_state_t) == 6, "sys_state_t size");
_Static_assert(sizeof(req_set_output_t) == 2, "req_set_output_t size");
_Static_assert(sizeof(evt_state_changed_t) == 7, "evt_state_changed_t size");
_Static_assert(sizeof(rsp_error_t) == 2, "rsp_error_t size");

/* ---------------- receiver: byte-by-byte state machine ---------------- */
typedef enum {
    PARSE_NONE,       /* need more bytes */
    PARSE_FRAME,      /* a valid frame is in parser->buf; type/seq/len are set */
    PARSE_CRC_ERROR,  /* frame dropped; type/seq tell which request failed */
    PARSE_LEN_ERROR,  /* LEN larger than the buffer; frame dropped */
} parse_result_t;

typedef struct {
    uint8_t *buf;     /* payload buffer, owned by the caller */
    size_t cap;
    int state;
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    uint16_t idx;
    uint16_t crc;     /* running CRC over TYPE..PAYLOAD */
    uint8_t crc_lo;
} proto_parser_t;

void proto_parser_init(proto_parser_t *p, uint8_t *buf, size_t cap);
void proto_parser_reset(proto_parser_t *p);
/* true while the parser is in the middle of a frame (used for the inter-byte timeout) */
bool proto_parser_busy(const proto_parser_t *p);
parse_result_t proto_parser_feed(proto_parser_t *p, uint8_t byte);

/* ---------------- transmitter ---------------- */
/* Writes the 6-byte header and returns the CRC over header and payload (send it low byte first). */
uint16_t proto_build_header(uint8_t hdr[PROTO_HEADER_SIZE], uint8_t type, uint8_t seq, const void *payload,
                            uint16_t len);
