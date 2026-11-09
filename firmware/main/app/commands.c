/* commands.c - request handlers. Runs alone on core 1 so inference is never interrupted by
 * display or RFID work, and the receive task on core 0 keeps filling the second buffer. */
#include "commands.h"

#include <inttypes.h>
#include <string.h>

#include "app_config.h"
#include "app_state.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture_filter.h"
#include "link.h"
#include "model.h"

#define N_PROFILE (NUM_CONV_LAYERS + 2)

static const char *TAG = "cmd";
static const char *const k_layer_names[N_PROFILE] = {"stem", "stage1_conv", "stage1_pw", "stage2_conv", "stage2_pw",
                                                     "stage3_conv", "stage3_pw", "gap", "fc"};

static gesture_filter_t s_filter;
static rsp_selftest_t s_post;
static uint32_t s_n_infer;
static uint32_t s_last_infer_us;
static uint32_t s_model_hash;
static uint32_t s_macs;

/* ---------------------------------------------------------------- model info */
static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* CRC32 over all parameters: the PC can check which model build is on the board. */
static void compute_model_info(void)
{
    uint32_t h = 0;
    uint64_t macs = 0;
    h = crc32_update(h, g_input_lut, sizeof(g_input_lut));
    for (int l = 0; l < NUM_CONV_LAYERS; l++) {
        const conv_layer_t *L = &g_conv_layers[l];
        const size_t n_w = (size_t)L->cout * L->k * L->k * L->cin;
        h = crc32_update(h, L->w, n_w);
        h = crc32_update(h, L->bias, L->cout * sizeof(int32_t));
        h = crc32_update(h, L->mult, L->cout * sizeof(int32_t));
        h = crc32_update(h, L->shift, L->cout * sizeof(int32_t));
        macs += (uint64_t)L->out_hw * L->out_hw * n_w;
    }
    h = crc32_update(h, g_fc_w, sizeof(g_fc_w));
    h = crc32_update(h, g_fc_b, sizeof(g_fc_b));
    macs += sizeof(g_fc_w);
    s_model_hash = h;
    s_macs = (uint32_t)macs;
}

/* ---------------------------------------------------------------- self-test */
static void run_golden(rsp_selftest_t *out)
{
    int32_t logits[NUM_CLASSES];
    const int img_bytes = IMG_W * IMG_H * IMG_C;
    uint64_t total = 0;
    memset(out, 0, sizeof(*out));
    out->min_us = UINT32_MAX;
    out->total = (uint8_t)g_num_test;

    for (int i = 0; i < g_num_test; i++) {
        const int64_t t0 = esp_timer_get_time();
        model_run(&g_test_rgb[i * img_bytes], logits);
        const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        const bool exact = memcmp(logits, &g_test_logits[i * NUM_CLASSES], sizeof(logits)) == 0;
        out->passed += exact;
        total += us;
        out->min_us = us < out->min_us ? us : out->min_us;
        out->max_us = us > out->max_us ? us : out->max_us;
        if (!exact) {
            ESP_LOGE(TAG, "golden image %d: logits differ from the reference model", i);
        }
        vTaskDelay(1);   /* let the idle task run (task watchdog) */
    }
    out->avg_us = g_num_test ? (uint32_t)(total / (uint64_t)g_num_test) : 0;
}

int commands_post(void)
{
    compute_model_info();
    run_golden(&s_post);
    ESP_LOGI(TAG, "POST: %u/%u golden images bit-exact, %.2f ms per image (min %.2f, max %.2f)", s_post.passed,
             s_post.total, s_post.avg_us / 1000.0, s_post.min_us / 1000.0, s_post.max_us / 1000.0);
    ESP_LOGI(TAG, "model hash 0x%08" PRIX32 ", %" PRIu32 " MACs per image", s_model_hash, s_macs);
    return s_post.passed;
}

/* ---------------------------------------------------------------- handlers */
static void handle_ping(const link_job_t *job)
{
    rsp_ping_t r = {
        .proto_version = PROTO_VERSION,
        .n_classes = NUM_CLASSES,
        .in_w = IMG_W,
        .in_h = IMG_H,
        .in_c = IMG_C,
        .n_layers = N_PROFILE,
        .model_hash = s_model_hash,
        .macs = s_macs,
        .cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
    };
    strncpy(r.backend, "ref-c", sizeof(r.backend) - 1);
    strncpy(r.model_name, MODEL_NAME, sizeof(r.model_name) - 1);
    strncpy(r.fw_version, FW_VERSION, sizeof(r.fw_version) - 1);
    link_send(MSG_RESPONSE(MSG_PING), job->seq, &r, sizeof(r));
}

static void handle_infer(const link_job_t *job)
{
    static uint8_t out[sizeof(rsp_infer_head_t) + NUM_CLASSES * sizeof(int32_t)];
    rsp_infer_head_t *r = (rsp_infer_head_t *)out;
    int32_t logits[NUM_CLASSES];

    const int64_t t0 = esp_timer_get_time();
    model_run(job->payload, logits);
    const uint32_t infer_us = (uint32_t)(esp_timer_get_time() - t0);

    const int cls = model_argmax(logits);
    const uint16_t conf = (uint16_t)(model_confidence(logits, cls) * 1000.0f + 0.5f);

    /* debounce, then let the state machine decide what to do */
    const int confirmed = gf_feed(&s_filter, cls, conf, (uint32_t)(esp_timer_get_time() / 1000));
    const action_t action = confirmed >= 0 ? app_state_on_gesture(confirmed) : ACT_NONE;
    app_state_set_prediction(cls, conf, gf_count(&s_filter));

    sys_state_t st;
    app_state_get(&st);

    r->class_id = (uint8_t)cls;
    r->n_classes = NUM_CLASSES;
    r->conf_permille = conf;
    r->infer_us = infer_us;
    r->action = (uint8_t)action;
    r->filter_count = gf_count(&s_filter);
    r->lock_state = st.lock_state;
    r->outputs = st.outputs;
    memcpy(out + sizeof(*r), logits, sizeof(logits));
    r->device_us = (uint32_t)(esp_timer_get_time() - job->rx_done_us);

    s_n_infer++;
    s_last_infer_us = infer_us;
    link_send(MSG_RESPONSE(MSG_INFER), job->seq, out, sizeof(out));
}

static void handle_selftest(const link_job_t *job)
{
    rsp_selftest_t r;
    run_golden(&r);
    link_send(MSG_RESPONSE(MSG_SELFTEST), job->seq, &r, sizeof(r));
}

static void handle_stats(const link_job_t *job)
{
    link_stats_t ls;
    link_get_stats(&ls);
    const rsp_stats_t r = {
        .uptime_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .n_frames_ok = ls.frames_ok,
        .n_infer = s_n_infer,
        .n_crc_err = ls.crc_err,
        .n_len_err = ls.len_err,
        .n_timeout = ls.timeout,
        .heap_internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .heap_internal_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        .psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .last_infer_us = s_last_infer_us,
        .post_passed = s_post.passed,
        .post_total = s_post.total,
    };
    link_send(MSG_RESPONSE(MSG_STATS), job->seq, &r, sizeof(r));
}

static void handle_profile(const link_job_t *job)
{
    rsp_profile_entry_t r[N_PROFILE];
    memset(r, 0, sizeof(r));
    for (int i = 0; i < N_PROFILE; i++) {
        strncpy(r[i].name, k_layer_names[i], sizeof(r[i].name) - 1);
        r[i].time_us = g_layer_us[i];
    }
    link_send(MSG_RESPONSE(MSG_PROFILE), job->seq, r, sizeof(r));
}

static void handle_get_state(const link_job_t *job)
{
    sys_state_t st;
    app_state_get(&st);
    link_send(MSG_RESPONSE(MSG_GET_STATE), job->seq, &st, sizeof(st));
}

static void handle_set_output(const link_job_t *job)
{
    const req_set_output_t *req = (const req_set_output_t *)job->payload;
    const fsm_status_t s = app_state_set_output(req->target, req->value);
    if (s == FSM_ERR_LOCKED) {
        link_send_error(job->seq, PROTO_ERR_LOCKED, job->type);
    } else if (s == FSM_ERR_ARG) {
        link_send_error(job->seq, PROTO_ERR_LEN, job->type);   /* value out of range for this TYPE */
    } else {
        handle_get_state(job);   /* response = new state; same layout as GET_STATE */
    }
}

/* Expected payload length for each request type (-1 = unknown type). */
static int expected_len(uint8_t type)
{
    switch (type) {
    case MSG_INFER:      return PROTO_IMAGE_BYTES;
    case MSG_SET_OUTPUT: return (int)sizeof(req_set_output_t);
    case MSG_PING:
    case MSG_SELFTEST:
    case MSG_STATS:
    case MSG_PROFILE:
    case MSG_GET_STATE:  return 0;
    default:             return -1;
    }
}

static void dispatch(const link_job_t *job)
{
    const int want = expected_len(job->type);
    if (want < 0) {
        link_send_error(job->seq, PROTO_ERR_UNKNOWN_CMD, job->type);
        return;
    }
    if (job->len != want) {
        link_send_error(job->seq, PROTO_ERR_LEN, job->type);
        return;
    }
    switch (job->type) {
    case MSG_PING:       handle_ping(job); break;
    case MSG_INFER:      handle_infer(job); break;
    case MSG_SELFTEST:   handle_selftest(job); break;
    case MSG_STATS:      handle_stats(job); break;
    case MSG_PROFILE:    handle_profile(job); break;
    case MSG_GET_STATE:  handle_get_state(job); break;
    case MSG_SET_OUTPUT: handle_set_output(job); break;
    default:             break;
    }
}

static void infer_task(void *arg)
{
    (void)arg;
    for (;;) {
        link_job_t *job = link_wait_job(portMAX_DELAY);
        if (job != NULL) {
            dispatch(job);
            link_release_job(job);
        }
    }
}

esp_err_t commands_start(void)
{
    const gesture_filter_cfg_t cfg = {
        .min_count = GESTURE_MIN_COUNT,
        .min_conf_permille = GESTURE_MIN_CONF_PERMILLE,
        .cooldown_ms = GESTURE_COOLDOWN_MS,
    };
    gf_init(&s_filter, &cfg);
    const BaseType_t ok = xTaskCreatePinnedToCore(infer_task, "infer", TASK_INFER_STACK, NULL, TASK_INFER_PRIO, NULL,
                                                  TASK_INFER_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
