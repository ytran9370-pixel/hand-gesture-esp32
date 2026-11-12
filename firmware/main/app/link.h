/* link.h - UART link to the PC: receives frames into a pool of job buffers and sends responses/events.
 *
 *   UART0 --> link_rx task --> parser --> job queue --> infer task (commands.c)
 *                                    ^                        |
 *                                    +---- free buffers <-----+
 *
 * Two job buffers let the next image arrive while the current one is being inferred.
 * If both are in use, the request is answered with ERROR(BUSY) and the PC retries. */
#pragma once
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "proto.h"

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    int64_t rx_done_us;                 /* time the last byte arrived (for device_us) */
    uint8_t payload[PROTO_MAX_PAYLOAD];
} link_job_t;

typedef struct {
    uint32_t frames_ok;
    uint32_t crc_err;
    uint32_t len_err;
    uint32_t timeout;
    uint32_t busy;
} link_stats_t;

esp_err_t link_init(void);
esp_err_t link_start(void);

/* Consumer side (infer task): wait for a job, then give the buffer back when done. */
link_job_t *link_wait_job(TickType_t timeout);
void link_release_job(link_job_t *job);

/* Thread-safe: may be called from any task. */
void link_send(uint8_t type, uint8_t seq, const void *payload, uint16_t len);
void link_send_error(uint8_t seq, uint8_t code, uint8_t req_type);
void link_send_event(const sys_state_t *state, uint8_t reason);

void link_get_stats(link_stats_t *out);
