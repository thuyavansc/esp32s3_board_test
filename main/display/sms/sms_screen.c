/**
 * sms_screen.c — SMS inbox + send GUI (see sms_screen.h for the design)
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"
#include "sms_client.h"
#include "gps_client.h"
#include "bg_worker.h"
#include "test_menu.h"
#include "text_keypad.h"
#include "toast.h"
#include "loading_overlay.h"
#include "display/ui_theme.h"
#include "display/ui_widgets.h"

static const char *TAG = "sms_ui";

static lv_obj_t *s_screen     = NULL;
static lv_obj_t *s_inbox_list = NULL;

static lv_timer_t *s_refresh_timer = NULL;

// ── Outbound send — single in-flight op, same guard shape as
//    network_screen.c's uplink-switch flow ──
static lv_timer_t *s_op_timer     = NULL;
static uint32_t    s_op_start_ms  = 0;
static bool         s_op_done_flag = false;
static bool         s_op_success   = false;
static char s_send_number[24];
static char s_send_message[161];

#define SMS_SEND_WATCHDOG_MS   25000

static void _back_to_test_menu(lv_event_t *e) {
    test_menu_return();
}

// ═══════════════════════════════════════════════════════════════
//  Inbox
// ═══════════════════════════════════════════════════════════════
static void _refresh_inbox(void) {
    if (!s_inbox_list) return;
    lv_obj_clean(s_inbox_list);

    sms_inbox_entry_t entries[SMS_INBOX_CAPACITY];
    int n = sms_client_get_inbox(entries, SMS_INBOX_CAPACITY);

    if (n == 0) {
        ui_label(s_inbox_list, "No SMS received yet", C_TEXT2);
        return;
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t *row = ui_card(s_inbox_list, 296, 76);

        char hdr[48];
        snprintf(hdr, sizeof(hdr), "%s%s", entries[i].sender, entries[i].was_command ? "   [CMD]" : "");
        lv_obj_t *hdr_lbl = ui_label(row, hdr, entries[i].was_command ? C_WARN : C_TEXT);
        lv_obj_align(hdr_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *body_lbl = ui_label(row, entries[i].body, C_TEXT2);
        lv_obj_set_width(body_lbl, 284);
        lv_label_set_long_mode(body_lbl, LV_LABEL_LONG_DOT);   // single line, truncated with "..." if too long
        lv_obj_align(body_lbl, LV_ALIGN_TOP_LEFT, 0, 24);

        char ts[16] = "-";
        if (entries[i].received_at > 0) {
            struct tm tmv;
            localtime_r(&entries[i].received_at, &tmv);
            strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
        }
        lv_obj_t *ts_lbl = ui_label(row, ts, C_TEXT2);
        lv_obj_align(ts_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

static void _refresh_timer_cb(lv_timer_t *timer) {
    if (lv_scr_act() != s_screen) return;   // cheap (RAM copy, no AT traffic) but still only bother while visible
    _refresh_inbox();
}

// ═══════════════════════════════════════════════════════════════
//  Send flow — two chained text_keypad_show() popups (number, then
//  message), then a bg_worker-routed gps_client_send_sms() with a
//  loading overlay + watchdog poll timer (same shape as
//  network_screen.c's uplink-switch flow / trip_screen.c's FETCH flow).
// ═══════════════════════════════════════════════════════════════
static bool _send_job_fn(void *arg) {
    (void)arg;
    return gps_client_send_sms(s_send_number, s_send_message, 20000);
}

static void _send_job_done(bool success, void *arg, void *user_data) {
    (void)arg; (void)user_data;
    s_op_success   = success;
    s_op_done_flag = true;
}

static void _send_poll_cb(lv_timer_t *timer) {
    if (s_op_done_flag) {
        loading_overlay_hide();
        toast_show(s_screen, s_op_success ? "SMS sent" : "SMS send failed \xE2\x9C\x97",
                   s_op_success ? TOAST_SUCCESS : TOAST_ERROR);
        lv_timer_del(s_op_timer);
        s_op_timer = NULL;
        return;
    }
    uint32_t elapsed_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_op_start_ms;
    if (elapsed_ms > SMS_SEND_WATCHDOG_MS) {
        ESP_LOGE(TAG, "SMS send: timed out after %lums with no response", (unsigned long)elapsed_ms);
        loading_overlay_hide();
        toast_show(s_screen, "Timed Out \xE2\x9C\x97", TOAST_ERROR);
        lv_timer_del(s_op_timer);
        s_op_timer = NULL;
    }
}

static void _on_message_entered(const char *value, void *user_data) {
    (void)user_data;
    if (!value || !value[0]) {
        toast_show(s_screen, "Message cannot be empty", TOAST_ERROR);
        return;
    }
    if (s_op_timer) {
        toast_show(s_screen, "A send is already in progress", TOAST_ERROR);
        return;
    }

    strlcpy(s_send_message, value, sizeof(s_send_message));
    loading_overlay_show(s_screen);
    s_op_done_flag = false;
    s_op_start_ms  = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (!bg_worker_submit_fn(_send_job_fn, NULL, _send_job_done, NULL)) {
        loading_overlay_hide();
        toast_show(s_screen, "Busy — try again", TOAST_ERROR);
        return;
    }
    s_op_timer = lv_timer_create(_send_poll_cb, 150, NULL);
}

static void _on_number_entered(const char *value, void *user_data) {
    (void)user_data;
    if (!value || !value[0]) {
        toast_show(s_screen, "Number cannot be empty", TOAST_ERROR);
        return;
    }
    strlcpy(s_send_number, value, sizeof(s_send_number));
    text_keypad_show(s_screen, "Message", "", false, _on_message_entered, NULL);
}

static void _send_btn_event(lv_event_t *e) {
    text_keypad_show(s_screen, "Recipient Number (e.g. +614...)", "", false, _on_number_entered, NULL);
}

// ═══════════════════════════════════════════════════════════════
//  Screen
// ═══════════════════════════════════════════════════════════════
static lv_obj_t *_create_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_back_header(scr, "SMS", _back_to_test_menu);

    lv_obj_t *send_btn = lv_btn_create(scr);
    lv_obj_set_size(send_btn, 296, 40);
    lv_obj_align(send_btn, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_bg_color(send_btn, C_ACCENT, 0);
    lv_obj_set_style_shadow_width(send_btn, 0, 0);
    lv_obj_set_style_radius(send_btn, 6, 0);
    lv_obj_t *send_lbl = ui_label(send_btn, "+ Send New SMS", C_TEXT);
    lv_obj_center(send_lbl);
    lv_obj_add_event_cb(send_btn, _send_btn_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = ui_label(scr, "Inbox (most recent first)", C_TEXT2);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 90);

    lv_obj_t *scroll = lv_obj_create(scr);
    lv_obj_set_size(scroll, 320, 480 - 112);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 8, 0);
    lv_obj_set_style_pad_row(scroll, 8, 0);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    s_inbox_list = scroll;

    return scr;
}

void sms_screen_create(void) {
    s_screen = _create_screen();
    s_refresh_timer = lv_timer_create(_refresh_timer_cb, 2000, NULL);
    ESP_LOGI(TAG, "SMS screen ready");
}

lv_obj_t *sms_screen_get_screen(void) {
    _refresh_inbox();
    return s_screen;
}
