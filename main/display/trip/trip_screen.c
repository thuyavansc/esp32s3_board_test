/**
 * trip_screen.c — Trip History
 *
 * Replaces the old hardcoded "FETCH #12772" / "LIST STORED" buttons and
 * placeholder card entirely:
 *   - A trip-number entry box (tap → numeric_keypad_show()) + FETCH button.
 *   - An always-visible, auto-populated trip list (no separate "LIST
 *     STORED" button needed — the list is just always there, sorted
 *     newest-first by rest_api_storage_list()).
 *   - FETCH shows the non-blocking loading_overlay while the ONE
 *     persistent background worker task (bg_worker.c) runs
 *     rest_api_storage_fetch() — ported from esp32_display_taxi_3, where this
 *     same pattern was shared with a GPS screen's SEND button (this
 *     project has no GPS hardware/screen); see bg_worker.h for why a
 *     shared persistent worker replaced spawning a fresh task per tap
 *     (that was failing with "out of memory" — see
 *     docs/TestFunctionalities/display/46_2026-07-09_memory_and_flash_analysis.md).
 *   - The most recently fetched trip's row is highlighted until a
 *     different trip is fetched.
 *   - Tapping a row opens trip_json_viewer_show().
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "trip_screen.h"
#include "trip_json_viewer.h"
#include "ui_main.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "backend/taximeter/rest_api_storage.h"
#include "ui_components/numeric_keypad.h"
#include "ui_components/loading_overlay.h"
#include "ui_components/toast.h"
#include "bg_worker.h"

static const char *TAG = "ui";

#define TRIP_LIST_MAX 32

static lv_obj_t *s_screen    = NULL;
static lv_obj_t *s_input_lbl = NULL;
static lv_obj_t *s_list_cont = NULL;
static char      s_trip_id_text[16] = "";
static int       s_last_fetched_id  = -1;

// ── Async FETCH state (same pattern as gps_screen.c's SEND) ──
typedef struct {
    volatile bool done;
    volatile bool success;
    int trip_id;
} fetch_result_t;

static fetch_result_t s_fetch_result   = {0};
static lv_timer_t    *s_poll_timer     = NULL;
static uint32_t       s_fetch_start_ms = 0;

// Client-side watchdog: rest_api_storage_fetch()'s own HTTP timeout
// (TRIPS_HTTP_TIMEOUT_MS, config.h) is 30s, so if nothing has come back
// after this much longer, something is wrong below the HTTP layer (task
// never started, hung DNS/TLS, etc.) — stop waiting and tell the user,
// rather than spinning forever with no feedback.
#define FETCH_WATCHDOG_MS 40000

// ═══════════════════════════════════════════════════════════════
//  Trip-number entry
// ═══════════════════════════════════════════════════════════════
static void _on_keypad_enter(const char *value, void *user_data) {
    (void)user_data;
    strlcpy(s_trip_id_text, value ? value : "", sizeof(s_trip_id_text));
    if (s_input_lbl) {
        lv_label_set_text(s_input_lbl, s_trip_id_text[0] ? s_trip_id_text : "Tap to enter");
    }
}

static void _input_box_click(lv_event_t *e) {
    numeric_keypad_show(s_screen, s_trip_id_text, _on_keypad_enter, NULL);
}

// ═══════════════════════════════════════════════════════════════
//  FETCH — background task + lv_timer polling (see gps_screen.c)
// ═══════════════════════════════════════════════════════════════
// Called from the bg_worker task's own context — NOT the LVGL thread.
// Only write to the plain result struct here; _fetch_poll_cb (on the
// LVGL thread) picks it up and does the actual overlay/list/toast work.
static void _on_fetch_job_done(bool success, int arg, void *user_data) {
    s_fetch_result.success = success;
    s_fetch_result.trip_id = arg;
    s_fetch_result.done    = true;
}

static void _fetch_finish(bool success, const char *toast_msg) {
    loading_overlay_hide();
    if (success) {
        s_last_fetched_id = s_fetch_result.trip_id;
        trip_screen_refresh_list();
    } else {
        toast_show(s_screen, toast_msg, TOAST_ERROR);
    }
    lv_timer_del(s_poll_timer);
    s_poll_timer = NULL;
}

static void _fetch_poll_cb(lv_timer_t *timer) {
    if (s_fetch_result.done) {
        ESP_LOGI(TAG, "Trip fetch #%d: %s", s_fetch_result.trip_id,
                 s_fetch_result.success ? "OK" : "FAILED");
        _fetch_finish(s_fetch_result.success, "Fetch Failed \xE2\x9C\x97");
        return;
    }

    uint32_t elapsed_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_fetch_start_ms;
    if (elapsed_ms > FETCH_WATCHDOG_MS) {
        ESP_LOGE(TAG, "Trip fetch #%d: TIMED OUT after %lums with no response — giving up",
                 s_fetch_result.trip_id, (unsigned long)elapsed_ms);
        _fetch_finish(false, "Fetch Timed Out \xE2\x9C\x97");
    }
}

static void _fetch_event(lv_event_t *e) {
    if (s_poll_timer) return;  // a fetch is already in flight

    int trip_id = atoi(s_trip_id_text);
    ESP_LOGI(TAG, "FETCH tapped — entered text: '%s' -> trip_id=%d", s_trip_id_text, trip_id);
    if (trip_id <= 0) {
        toast_show(s_screen, "Enter a valid trip number", TOAST_ERROR);
        return;
    }

    loading_overlay_show(s_screen);
    s_fetch_result.done    = false;
    s_fetch_result.trip_id = trip_id;
    s_fetch_start_ms       = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (!bg_worker_submit(BG_JOB_TRIP_FETCH, trip_id, _on_fetch_job_done, NULL)) {
        ESP_LOGE(TAG, "FETCH: background worker busy — try again in a moment (trip #%d)", trip_id);
        loading_overlay_hide();
        toast_show(s_screen, "Fetch Failed \xE2\x9C\x97", TOAST_ERROR);
        return;
    }

    s_poll_timer = lv_timer_create(_fetch_poll_cb, 150, NULL);
}

// ═══════════════════════════════════════════════════════════════
//  List row tap → JSON viewer
// ═══════════════════════════════════════════════════════════════
static void _row_click(lv_event_t *e) {
    int trip_id = (int)(intptr_t)lv_event_get_user_data(e);
    trip_json_viewer_show(trip_id);
}

// ═══════════════════════════════════════════════════════════════
//  List (re)build
// ═══════════════════════════════════════════════════════════════
void trip_screen_refresh_list(void) {
    if (!s_list_cont) return;
    lv_obj_clean(s_list_cont);

    static trip_file_info_t list[TRIP_LIST_MAX];
    int count = rest_api_storage_list(list, TRIP_LIST_MAX);

    if (count == 0) {
        lv_obj_t *empty_lbl = ui_label(s_list_cont,
            "No trips stored yet.\nEnter a trip number and tap FETCH.", C_TEXT2);
        lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        bool highlighted = (list[i].trip_id == s_last_fetched_id);

        lv_obj_t *row = lv_obj_create(s_list_cont);
        lv_obj_set_size(row, 296, 48);
        lv_obj_set_style_bg_color(row, highlighted ? C_SUCCESS : C_CARD, 0);
        lv_obj_set_style_border_width(row, highlighted ? 0 : 1, 0);
        lv_obj_set_style_border_color(row, C_DIVIDER, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, _row_click, LV_EVENT_CLICKED, (void *)(intptr_t)list[i].trip_id);

        lv_color_t fg = highlighted ? C_BG : C_TEXT;
        lv_color_t fg2 = highlighted ? C_BG : C_TEXT2;

        char id_buf[24];
        snprintf(id_buf, sizeof(id_buf), "Trip #%d", list[i].trip_id);
        lv_obj_t *id_lbl = ui_label(row, id_buf, fg);
        lv_obj_align(id_lbl, LV_ALIGN_LEFT_MID, 2, -9);

        struct tm ti;
        localtime_r(&list[i].mtime, &ti);
        // 5 plain-int %d fields — GCC's format-truncation check assumes each could
        // be up to 11 chars (INT_MIN), so worst case is ~60 bytes; size well above that.
        char date_buf[72];
        snprintf(date_buf, sizeof(date_buf), "%02d/%02d/%04d %02d:%02d",
                 ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900, ti.tm_hour, ti.tm_min);
        lv_obj_t *date_lbl = ui_label(row, date_buf, fg2);
        lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_10, 0);
        lv_obj_align(date_lbl, LV_ALIGN_LEFT_MID, 2, 10);

        lv_obj_t *chevron = ui_label(row, LV_SYMBOL_RIGHT, fg2);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════
lv_obj_t *trip_screen_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 32);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_ACCENT, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    ui_label(hdr, "TRIP HISTORY", C_TEXT);
    lv_obj_align(lv_obj_get_child(hdr, 0), LV_ALIGN_CENTER, 0, 0);

    // Entry row: label + tappable box + FETCH
    lv_obj_t *entry_lbl = ui_label(scr, "Enter Trip Number", C_TEXT2);
    lv_obj_align(entry_lbl, LV_ALIGN_TOP_LEFT, 10, 38);

    lv_obj_t *input_box = ui_card(scr, 190, 40);
    lv_obj_align(input_box, LV_ALIGN_TOP_LEFT, 8, 58);
    lv_obj_add_flag(input_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(input_box, _input_box_click, LV_EVENT_CLICKED, NULL);
    s_input_lbl = ui_label(input_box, "Tap to enter", C_TEXT2);
    lv_obj_center(s_input_lbl);

    lv_obj_t *fetch_btn = lv_btn_create(scr);
    lv_obj_set_size(fetch_btn, 92, 40);
    lv_obj_align(fetch_btn, LV_ALIGN_TOP_RIGHT, -8, 58);
    lv_obj_set_style_bg_color(fetch_btn, C_SUCCESS, 0);
    lv_obj_set_style_shadow_width(fetch_btn, 0, 0);
    lv_obj_set_style_radius(fetch_btn, 8, 0);
    lv_obj_t *fetch_lbl = ui_label(fetch_btn, "FETCH", C_BG);
    lv_obj_center(fetch_lbl);
    lv_obj_add_event_cb(fetch_btn, _fetch_event, LV_EVENT_CLICKED, NULL);

    // Scrollable trip list — same flex-column scroll pattern as Settings
    lv_obj_t *list_cont = lv_obj_create(scr);
    lv_obj_set_size(list_cont, 320, 322);  // 480 - 106 (header+entry row) - 52 (nav bar)
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, 106);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 8, 0);
    lv_obj_set_style_pad_row(list_cont, 6, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_cont,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);
    lv_obj_add_flag(list_cont, LV_OBJ_FLAG_SCROLLABLE);
    s_list_cont = list_cont;

    ui_add_nav_bar(scr, SCREEN_TRIPS);
    s_screen = scr;

    trip_json_viewer_create();
    trip_screen_refresh_list();

    return scr;
}

lv_obj_t *trip_screen_get_screen(void) {
    return s_screen;
}
