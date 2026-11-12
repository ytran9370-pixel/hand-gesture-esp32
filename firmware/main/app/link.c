/* link.c - UART0 driver, receive task and frame transmitter. */
#include "link.h"

#include <string.h>

#include "app_config.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define UART_RX_RING 4096
#define UART_TX_RING 4096

static const char *TAG = "link";

static link_job_t s_jobs[LINK_JOB_BUFFERS];
static QueueHandle_t s_free_q;   /* buffers ready to be filled */
static QueueHandle_t s_job_q;    /* filled buffers waiting for the infer task */
static SemaphoreHandle_t s_tx_mutex;
static uint8_t s_evt_seq;

static proto_parser_t s_parser;
static uint8_t s_rx_payload[PROTO_MAX_PAYLOAD];
static link_stats_t s_stats;     /* written only by the rx task; 32-bit reads are atomic */

esp_err_t link_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(LINK_UART_NUM, UART_RX_RING, UART_TX_RING, 0, NULL, 0), TAG, "install");
    ESP_RETURN_ON_ERROR(uart_param_config(LINK_UART_NUM, &cfg), TAG, "config");
    ESP_RETURN_ON_ERROR(uart_set_pin(LINK_UART_NUM, PIN_UART_TX, PIN_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "pins");

    s_free_q = xQueueCreate(LINK_JOB_BUFFERS, sizeof(link_job_t *));
    s_job_q = xQueueCreate(LINK_JOB_BUFFERS, sizeof(link_job_t *));
    s_tx_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_free_q && s_job_q && s_tx_mutex, ESP_ERR_NO_MEM, TAG, "queues");
    for (int i = 0; i < LINK_JOB_BUFFERS; i++) {
        link_job_t *job = &s_jobs[i];
        xQueueSend(s_free_q, &job, 0);
    }
    proto_parser_init(&s_parser, s_rx_payload, sizeof(s_rx_payload));
    ESP_LOGI(TAG, "UART%d %d baud, TX=%d RX=%d", LINK_UART_NUM, LINK_BAUD, PIN_UART_TX, PIN_UART_RX);
    return ESP_OK;
}

/* ---------------------------------------------------------------- transmit */
void link_send(uint8_t type, uint8_t seq, const void *payload, uint16_t len)
{
    uint8_t hdr[PROTO_HEADER_SIZE];
    const uint16_t crc = proto_build_header(hdr, type, seq, payload, len);
    const uint8_t tail[PROTO_CRC_SIZE] = {(uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8)};

    /* one task at a time, so frames from different tasks never interleave */
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(LINK_UART_NUM, hdr, sizeof(hdr));
    if (len > 0) {
        uart_write_bytes(LINK_UART_NUM, payload, len);
    }
    uart_write_bytes(LINK_UART_NUM, tail, sizeof(tail));
    xSemaphoreGive(s_tx_mutex);
}

void link_send_error(uint8_t seq, uint8_t code, uint8_t req_type)
{
    const rsp_error_t err = {.code = code, .req_type = req_type};
    link_send(MSG_ERROR, seq, &err, sizeof(err));
}

void link_send_event(const sys_state_t *state, uint8_t reason)
{
    const evt_state_changed_t evt = {.state = *state, .reason = reason};
    const uint8_t seq = __atomic_fetch_add(&s_evt_seq, 1, __ATOMIC_RELAXED);   /* called from several tasks */
    link_send(MSG_EVT_STATE_CHANGED, seq, &evt, sizeof(evt));
}

/* ---------------------------------------------------------------- receive */
static void on_frame(int64_t now_us)
{
    link_job_t *job = NULL;
    if (xQueueReceive(s_free_q, &job, 0) != pdTRUE) {
        s_stats.busy++;
        link_send_error(s_parser.seq, PROTO_ERR_BUSY, s_parser.type);
        return;
    }
    job->type = s_parser.type;
    job->seq = s_parser.seq;
    job->len = s_parser.len;
    job->rx_done_us = now_us;
    memcpy(job->payload, s_rx_payload, s_parser.len);
    s_stats.frames_ok++;
    xQueueSend(s_job_q, &job, 0);   /* cannot fail: the queue holds every buffer */
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[256];
    int64_t last_byte_us = 0;

    for (;;) {
        const int n = uart_read_bytes(LINK_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(2));
        const int64_t now_us = esp_timer_get_time();

        if (n <= 0) {
            /* FR-COM-04: a frame that stops for more than 50 ms is dropped */
            if (proto_parser_busy(&s_parser) && now_us - last_byte_us > LINK_RX_TIMEOUT_MS * 1000) {
                proto_parser_reset(&s_parser);
                s_stats.timeout++;
                link_send_error(0, PROTO_ERR_TIMEOUT, 0);
            }
            continue;
        }
        last_byte_us = now_us;

        for (int i = 0; i < n; i++) {
            switch (proto_parser_feed(&s_parser, chunk[i])) {
            case PARSE_FRAME:
                on_frame(now_us);
                break;
            case PARSE_CRC_ERROR:
                s_stats.crc_err++;
                link_send_error(s_parser.seq, PROTO_ERR_CRC, s_parser.type);
                break;
            case PARSE_LEN_ERROR:
                s_stats.len_err++;
                link_send_error(s_parser.seq, PROTO_ERR_LEN, s_parser.type);
                break;
            case PARSE_NONE:
            default:
                break;
            }
        }
    }
}

esp_err_t link_start(void)
{
    const BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "link_rx", TASK_LINK_RX_STACK, NULL, TASK_LINK_RX_PRIO,
                                                  NULL, TASK_LINK_RX_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

link_job_t *link_wait_job(TickType_t timeout)
{
    link_job_t *job = NULL;
    return xQueueReceive(s_job_q, &job, timeout) == pdTRUE ? job : NULL;
}

void link_release_job(link_job_t *job)
{
    xQueueSend(s_free_q, &job, 0);
}

void link_get_stats(link_stats_t *out)
{
    *out = s_stats;
}
