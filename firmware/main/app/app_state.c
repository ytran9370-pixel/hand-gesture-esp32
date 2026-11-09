/* app_state.c - see app_state.h. */
#include "app_state.h"

#include <ctype.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gesture_filter.h"
#include "link.h"
#include "model.h"
#include "outputs.h"

#define MAX_CARDS 8

static const char *TAG = "state";
static SemaphoreHandle_t s_mutex;
static system_fsm_t s_fsm;
static app_view_t s_view;
static uint32_t s_denied_until_ms;
static uint8_t s_cards[MAX_CARDS][4];
static int s_n_cards;

static const char *const k_lock_names[] = {"LOCKED", "UNLOCKED", "LOCKOUT"};
static const char *const k_reason_names[] = {"-", "card ok", "card denied", "gesture", "auto-lock", "PC command",
                                             "lockout end"};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* "A1B2C3D4, 0e5f6071" -> list of 4-byte UIDs. Invalid entries are skipped with a warning. */
static void parse_whitelist(const char *s)
{
    while (*s != '\0' && s_n_cards < MAX_CARDS) {
        uint8_t uid[4] = {0};
        int nibbles = 0;
        for (; *s != '\0' && *s != ','; s++) {
            if (isxdigit((unsigned char)*s) && nibbles < 8) {
                const int v = isdigit((unsigned char)*s) ? *s - '0' : (tolower((unsigned char)*s) - 'a' + 10);
                uid[nibbles / 2] = (uint8_t)((uid[nibbles / 2] << 4) | v);
                nibbles++;
            }
        }
        if (nibbles == 8) {
            memcpy(s_cards[s_n_cards++], uid, 4);
        } else if (nibbles > 0) {
            ESP_LOGW(TAG, "whitelist entry ignored: need 8 hex digits");
        }
        if (*s == ',') {
            s++;
        }
    }
}

static bool is_authorized(const uint8_t uid[4])
{
    for (int i = 0; i < s_n_cards; i++) {
        if (memcmp(s_cards[i], uid, 4) == 0) {
            return true;
        }
    }
    return false;
}

static void fill_state(sys_state_t *st, uint32_t t)
{
    st->lock_state = (uint8_t)s_fsm.lock;
    st->outputs = s_fsm.outputs;
    st->last_gesture = s_fsm.last_gesture;
    st->denied_count = s_fsm.denied_count;
    st->unlock_remaining_s = fsm_remaining_s(&s_fsm, t);
}

/* Called with the mutex held after the FSM changed: drive outputs and prepare the event.
 * The event itself is sent after the mutex is released (never block other tasks on the UART). */
static void after_change(fsm_reason_t reason, uint32_t t, sys_state_t *evt)
{
    outputs_apply(s_fsm.outputs);
    fill_state(evt, t);
    ESP_LOGI(TAG, "%s (%s) outputs=0x%02X", k_lock_names[s_fsm.lock], k_reason_names[reason], s_fsm.outputs);
}

void app_state_init(bool rfid_ok)
{
    s_mutex = xSemaphoreCreateMutex();
    const fsm_cfg_t cfg = {
        .unlock_timeout_ms = UNLOCK_TIMEOUT_MS,
        .lockout_ms = LOCKOUT_MS,
        .max_denied = MAX_DENIED_CARDS,
    };
    fsm_init(&s_fsm, &cfg);
    s_view.pred_cls = -1;
    s_view.rfid_ok = rfid_ok;
    parse_whitelist(CONFIG_HG_RFID_WHITELIST);
    ESP_LOGI(TAG, "%d authorized card(s) in the whitelist", s_n_cards);
    if (s_n_cards == 0) {
        ESP_LOGW(TAG, "whitelist is empty: tap a card, read its UID on the OLED, add it to CONFIG_HG_RFID_WHITELIST");
    }
}

void app_state_on_card(const uint8_t uid[4])
{
    const uint32_t t = now_ms();
    const bool ok = is_authorized(uid);
    ESP_LOGI(TAG, "card %02X%02X%02X%02X %s", uid[0], uid[1], uid[2], uid[3], ok ? "authorized" : "UNKNOWN");

    sys_state_t evt;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const fsm_reason_t r = fsm_on_card(&s_fsm, ok, t);
    if (!ok) {
        s_view.show_denied = true;
        memcpy(s_view.denied_uid, uid, 4);
        s_denied_until_ms = t + DENIED_MESSAGE_MS;
    }
    if (r != FSM_RSN_NONE) {
        after_change(r, t, &evt);
    }
    xSemaphoreGive(s_mutex);

    if (r != FSM_RSN_NONE) {
        link_send_event(&evt, (uint8_t)r);
    }
}

action_t app_state_on_gesture(int cls)
{
    const uint32_t t = now_ms();
    action_t action = ACT_NONE;
    sys_state_t evt;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const fsm_reason_t r = fsm_on_gesture(&s_fsm, cls, t, &action);
    if (r != FSM_RSN_NONE) {
        after_change(r, t, &evt);
    }
    xSemaphoreGive(s_mutex);

    if (r != FSM_RSN_NONE) {
        ESP_LOGI(TAG, "gesture '%s' -> action %d", g_labels[cls], action);
        link_send_event(&evt, (uint8_t)r);
    }
    return action;
}

fsm_status_t app_state_set_output(uint8_t target, uint8_t value)
{
    const uint32_t t = now_ms();
    sys_state_t evt;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const fsm_status_t st = fsm_set_output(&s_fsm, target, value, t);
    if (st == FSM_OK) {
        after_change(FSM_RSN_HOST_COMMAND, t, &evt);
    }
    xSemaphoreGive(s_mutex);

    if (st == FSM_OK) {
        link_send_event(&evt, FSM_RSN_HOST_COMMAND);
    }
    return st;
}

void app_state_tick(void)
{
    const uint32_t t = now_ms();
    sys_state_t evt;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const fsm_reason_t r = fsm_tick(&s_fsm, t);
    if (r != FSM_RSN_NONE) {
        after_change(r, t, &evt);
    }
    if (s_view.show_denied && time_reached(t, s_denied_until_ms)) {
        s_view.show_denied = false;
    }
    xSemaphoreGive(s_mutex);

    if (r != FSM_RSN_NONE) {
        link_send_event(&evt, (uint8_t)r);
    }
}

void app_state_set_prediction(int cls, uint16_t conf_permille, uint8_t filter_count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_view.pred_cls = cls;
    s_view.pred_conf_permille = conf_permille;
    s_view.filter_count = filter_count;
    xSemaphoreGive(s_mutex);
}

void app_state_get(sys_state_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    fill_state(out, now_ms());
    xSemaphoreGive(s_mutex);
}

void app_state_view(app_view_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    fill_state(&s_view.state, now_ms());
    *out = s_view;
    xSemaphoreGive(s_mutex);
}
