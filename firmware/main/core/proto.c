/* proto.c - frame receiver (state machine) and header builder. */
#include "proto.h"

#include "crc16.h"

enum { ST_SOF0, ST_SOF1, ST_TYPE, ST_SEQ, ST_LEN0, ST_LEN1, ST_PAYLOAD, ST_CRC0, ST_CRC1 };

void proto_parser_init(proto_parser_t *p, uint8_t *buf, size_t cap)
{
    p->buf = buf;
    p->cap = cap;
    proto_parser_reset(p);
}

void proto_parser_reset(proto_parser_t *p)
{
    p->state = ST_SOF0;
    p->idx = 0;
}

bool proto_parser_busy(const proto_parser_t *p)
{
    return p->state != ST_SOF0;
}

static void crc_add(proto_parser_t *p, uint8_t b)
{
    p->crc = crc16_ccitt(&b, 1, p->crc);
}

parse_result_t proto_parser_feed(proto_parser_t *p, uint8_t b)
{
    switch (p->state) {
    case ST_SOF0:
        if (b == PROTO_SOF0) {
            p->state = ST_SOF1;
        }
        break;
    case ST_SOF1:
        /* "A5 A5 5A" must still be found, so a repeated A5 keeps us waiting for 5A */
        p->state = (b == PROTO_SOF1) ? ST_TYPE : (b == PROTO_SOF0 ? ST_SOF1 : ST_SOF0);
        break;
    case ST_TYPE:
        p->crc = CRC16_INIT;
        crc_add(p, b);
        p->type = b;
        p->state = ST_SEQ;
        break;
    case ST_SEQ:
        crc_add(p, b);
        p->seq = b;
        p->state = ST_LEN0;
        break;
    case ST_LEN0:
        crc_add(p, b);
        p->len = b;
        p->state = ST_LEN1;
        break;
    case ST_LEN1:
        crc_add(p, b);
        p->len |= (uint16_t)(b << 8);
        if (p->len > p->cap) {
            p->state = ST_SOF0;   /* resynchronise on the next A5 5A */
            return PARSE_LEN_ERROR;
        }
        p->idx = 0;
        p->state = (p->len == 0) ? ST_CRC0 : ST_PAYLOAD;
        break;
    case ST_PAYLOAD:
        crc_add(p, b);
        p->buf[p->idx++] = b;
        if (p->idx == p->len) {
            p->state = ST_CRC0;
        }
        break;
    case ST_CRC0:
        p->crc_lo = b;
        p->state = ST_CRC1;
        break;
    case ST_CRC1:
    default: {
        const uint16_t rx = (uint16_t)(p->crc_lo | (b << 8));
        p->state = ST_SOF0;
        return rx == p->crc ? PARSE_FRAME : PARSE_CRC_ERROR;
    }
    }
    return PARSE_NONE;
}

uint16_t proto_build_header(uint8_t hdr[PROTO_HEADER_SIZE], uint8_t type, uint8_t seq, const void *payload,
                            uint16_t len)
{
    hdr[0] = PROTO_SOF0;
    hdr[1] = PROTO_SOF1;
    hdr[2] = type;
    hdr[3] = seq;
    hdr[4] = (uint8_t)(len & 0xFF);
    hdr[5] = (uint8_t)(len >> 8);
    uint16_t crc = crc16_ccitt(&hdr[2], 4, CRC16_INIT);
    return crc16_ccitt((const uint8_t *)payload, len, crc);
}
