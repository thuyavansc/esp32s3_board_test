/**
 * trip_screen.c — Trips: History (auto-load) + Manual Fetch (doc 188)
 *
 * Two tabs now, matching what you asked for after running 5+ real
 * trips and only ever seeing one record:
 *
 *   HISTORY (default tab) — auto-loads completed trips the moment this
 *   screen is shown, no trip-ID typing, matching the REAL Android
 *   TripHistoryFragment11 (confirmed against features/trip_sync/
 *   usecases/GetTripHistoryUseCase.kt + TripApi.kt — not guessed): one
 *   POST Job/GetAllBySearch returns a whole page of the driver's own
 *   completed ("Dropedoff") trips. This is what was missing — the old
 *   single-tab screen only ever showed trips reached via manual
 *   fetch-by-ID, which is why running 5 real trips never grew the list.
 *
 *   MANUAL FETCH — the previous screen's entire behavior, unchanged:
 *   type a trip number, FETCH (GET Trips/{id}), see it added to a
 *   locally-stored list. Kept as its own tab rather than removed,
 *   since it's still useful for pulling a SPECIFIC trip id directly
 *   (e.g. one a dispatcher just quoted over the phone) without paging
 *   through history to find it.
 *
 * Both tabs open the same trip_json_viewer_show(id) on a row tap —
 * history rows are pre-cached (trip_sync_fetch_history() already
 * writes each row's full JSON via rest_api_storage_write(), doc 188),
 * so opening one is instant, no extra network round-trip.
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
#include "esp_heap_caps.h"
#include "trip_screen.h"
#include "trip_json_viewer.h"
#include "ui_main.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "backend/taximeter/rest_api_storage.h"
#include "backend/taximeter/trip_sync.h"
#include "ui_components/numeric_keypad.h"
#include "ui_components/loading_overlay.h"
#include "ui_components/toast.h"
#include "bg_worker.h"

static const char *TAG = "ui";

#define TRIP_LIST_MAX 32

static lv_obj_t *s_screen    = NULL;

// ═══════════════════════════════════════════════════════════════
//  Tab switching
// ═══════════════════════════════════════════════════════════════
typedef enum { TRIP_TAB_HISTORY = 0, TRIP_TAB_MANUAL } trip_tab_t;
static trip_tab_t s_active_tab = TRIP_TAB_HISTORY;

static lv_obj_t *s_tab_history_btn = NULL;
static lv_obj_t *s_tab_manual_btn  = NULL;
static lv_obj_t *s_history_cont    = NULL;
static lv_obj_t *s_manual_cont     = NULL;

static void _apply_trip_tab_styles(void) {
    bool history_active = (s_active_tab == TRIP_TAB_HISTORY);
    if (s_history_cont) {
        if (history_active) lv_obj_clear_flag(s_history_cont, LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(s_history_cont, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_manual_cont) {
        if (!history_active) lv_obj_clear_flag(s_manual_cont, LV_OBJ_FLAG_HIDDEN);
        else                  lv_obj_add_flag(s_manual_cont, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_tab_history_btn) lv_obj_set_style_bg_color(s_tab_history_btn, history_active ? C_ACCENT : C_BTN, 0);
    if (s_tab_manual_btn)  lv_obj_set_style_bg_color(s_tab_manual_btn, !history_active ? C_ACCENT : C_BTN, 0);
}

static void _history_start_fetch(bool reset);  // fwd decl — used by the tab click below

static void _tab_history_event(lv_event_t *e) {
    (void)e;
    s_active_tab = TRIP_TAB_HISTORY;
    _apply_trip_tab_styles();
}

static void _tab_manual_event(lv_event_t *e) {
    (void)e;
    s_active_tab = TRIP_TAB_MANUAL;
    _apply_trip_tab_styles();
}

// Forward decls — _row_click is shared by both tabs but only defined
// once, in the Manual tab section below; _history_load_more_event is
// defined after _history_render() but referenced inside it.
static void _row_click(lv_event_t *e);
static void _history_load_more_event(lv_event_t *e);

// ═══════════════════════════════════════════════════════════════
//  HISTORY TAB — POST Job/GetAllBySearch, auto-loaded (doc 188)
// ═══════════════════════════════════════════════════════════════
#define HISTORY_PAGE_SIZE   10
#define HISTORY_MAX_LOADED  60   // 6 pages' worth before "Load More" stops offering more

static lv_obj_t *s_history_list_cont = NULL;
static lv_obj_t *s_history_refresh_btn = NULL;

// PSRAM-backed, not a static array — same "large lookup-style buffer
// that isn't touched from a tight/interrupt path -> PSRAM, not internal
// SRAM" precedent as reference_data.c's public-holidays move (doc 185).
// 60 * sizeof(trip_history_item_t) is ~16KB; small next to PSRAM's 8MB,
// but no reason to spend internal DRAM on it either.
static trip_history_item_t *s_history_items = NULL;
static int  s_history_loaded_count = 0;
static int  s_history_total_count  = 0;

// Async fetch state — same "job fills a plain struct on the worker
// task, an lv_timer on the LVGL thread does the actual UI work" split
// bg_worker.h's own header comment requires (and the Manual tab below
// already follows).
typedef struct {
    volatile bool done;
    int  page_number;
    trip_history_item_t page_items[HISTORY_PAGE_SIZE];
    int  page_count;
    int  total_count;
} history_job_t;

static history_job_t  s_history_job;
static lv_timer_t    *s_history_poll_timer   = NULL;
static uint32_t       s_history_fetch_start_ms = 0;
static bool           s_history_fetch_in_flight = false;

// Same 40s outside-HTTP-timeout watchdog rationale as the Manual tab's
// FETCH_WATCHDOG_MS below — trip_sync_fetch_history()'s own HTTP
// timeout is shorter; this only fires if something below that hung.
#define HISTORY_FETCH_WATCHDOG_MS 40000

static void _history_render(void) {
    if (!s_history_list_cont) return;
    lv_obj_clean(s_history_list_cont);

    if (!s_history_items) {
        lv_obj_t *err_lbl = ui_label(s_history_list_cont, "Trip history unavailable (out of PSRAM)", C_ERROR);
        lv_obj_set_style_text_align(err_lbl, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    if (s_history_loaded_count == 0) {
        const char *msg = s_history_fetch_in_flight
            ? "Loading trip history..."
            : "No completed trips found yet.";
        lv_obj_t *empty_lbl = ui_label(s_history_list_cont, msg, C_TEXT2);
        lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (int i = 0; i < s_history_loaded_count; i++) {
        const trip_history_item_t *it = &s_history_items[i];

        lv_obj_t *row = lv_obj_create(s_history_list_cont);
        lv_obj_set_size(row, 296, 58);
        lv_obj_set_style_bg_color(row, C_CARD, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, C_DIVIDER, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, _row_click, LV_EVENT_CLICKED, (void *)(intptr_t)it->id);

        char id_buf[24];
        snprintf(id_buf, sizeof(id_buf), "Trip #%lld", (long long)it->id);
        lv_obj_t *id_lbl = ui_label(row, id_buf, C_TEXT);
        lv_obj_align(id_lbl, LV_ALIGN_TOP_LEFT, 2, -8);

        char fare_buf[24];
        snprintf(fare_buf, sizeof(fare_buf), "$%.2f", it->total_fares);
        lv_obj_t *fare_lbl = ui_label(row, fare_buf, C_ACCENT2);
        lv_obj_align(fare_lbl, LV_ALIGN_TOP_RIGHT, -2, -8);

        // From city -> To city (fallback to "--" the same way
        // trip_json_viewer.c's K_PICKUP_ADDR/K_DROPOFF_ADDR fallback
        // chain does, for the same "don't show empty text" reason).
        char route_buf[72];
        snprintf(route_buf, sizeof(route_buf), "%s -> %s",
                 it->from_city[0] ? it->from_city : "--",
                 it->to_city[0] ? it->to_city : "--");
        lv_obj_t *route_lbl = ui_label(row, route_buf, C_TEXT2);
        lv_obj_set_style_text_font(route_lbl, &lv_font_montserrat_10, 0);
        lv_obj_align(route_lbl, LV_ALIGN_LEFT_MID, 2, 4);

        char time_buf[40];
        snprintf(time_buf, sizeof(time_buf), "%s",
                 it->dropoff_time[0] ? it->dropoff_time : (it->pickup_time[0] ? it->pickup_time : "--"));
        lv_obj_t *time_lbl = ui_label(row, time_buf, C_TEXT2);
        lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_10, 0);
        lv_obj_align(time_lbl, LV_ALIGN_BOTTOM_LEFT, 2, 6);

        lv_obj_t *chevron = ui_label(row, LV_SYMBOL_RIGHT, C_TEXT2);
        lv_obj_align(chevron, LV_ALIGN_BOTTOM_RIGHT, 0, 4);
    }

    if (s_history_loaded_count < s_history_total_count) {
        lv_obj_t *more_btn = lv_btn_create(s_history_list_cont);
        lv_obj_set_size(more_btn, 296, 40);
        lv_obj_set_style_bg_color(more_btn, C_BTN, 0);
        lv_obj_set_style_shadow_width(more_btn, 0, 0);
        lv_obj_set_style_radius(more_btn, 8, 0);
        lv_obj_t *more_lbl = ui_label(more_btn, "Load More", C_TEXT);
        lv_obj_center(more_lbl);
        lv_obj_add_event_cb(more_btn, _history_load_more_event, LV_EVENT_CLICKED, NULL);
    }
}

static void _history_job_fn_done(bool success, void *arg, void *user_data) {
    (void)success; (void)arg; (void)user_data;
    s_history_job.done = true;   // worker-task context — plain flag only, no LVGL calls (bg_worker.h's rule)
}

static bool _history_fetch_job(void *arg) {
    history_job_t *job = (history_job_t *)arg;
    job->page_count = trip_sync_fetch_history(job->page_number, HISTORY_PAGE_SIZE,
                                               job->page_items, HISTORY_PAGE_SIZE, &job->total_count);
    return true;   // trip_sync_fetch_history() already logs the real failure reason to serial; an empty page isn't necessarily an error (e.g. genuinely no completed trips yet)
}

static void _history_poll_cb(lv_timer_t *timer) {
    if (s_history_job.done) {
        s_history_fetch_in_flight = false;
        loading_overlay_hide();

        int add = s_history_job.page_count;
        for (int i = 0; i < add && s_history_items && s_history_loaded_count < HISTORY_MAX_LOADED; i++) {
            s_history_items[s_history_loaded_count++] = s_history_job.page_items[i];
        }
        s_history_total_count = s_history_job.total_count;
        ESP_LOGI(TAG, "History fetch page %d: +%d item(s), loaded=%d/%d",
                 s_history_job.page_number, add, s_history_loaded_count, s_history_total_count);

        _history_render();
        lv_timer_del(s_history_poll_timer);
        s_history_poll_timer = NULL;
        return;
    }

    uint32_t elapsed_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_history_fetch_start_ms;
    if (elapsed_ms > HISTORY_FETCH_WATCHDOG_MS) {
        ESP_LOGE(TAG, "History fetch: TIMED OUT after %lums with no response — giving up", (unsigned long)elapsed_ms);
        s_history_fetch_in_flight = false;
        loading_overlay_hide();
        toast_show(s_screen, "Trip history load timed out", TOAST_ERROR);
        _history_render();
        lv_timer_del(s_history_poll_timer);
        s_history_poll_timer = NULL;
    }
}

// reset=true starts over from page 1 (screen just shown / pull-to-refresh);
// reset=false continues from wherever s_history_loaded_count left off
// ("Load More").
static void _history_start_fetch(bool reset) {
    if (!s_history_items || s_history_fetch_in_flight) return;
    if (reset) {
        s_history_loaded_count = 0;
        s_history_total_count  = 0;
        _history_render();   // shows "Loading..." immediately instead of the stale previous list
    }

    s_history_job.done        = false;
    s_history_job.page_number = (s_history_loaded_count / HISTORY_PAGE_SIZE) + 1;
    s_history_fetch_in_flight = true;
    s_history_fetch_start_ms  = xTaskGetTickCount() * portTICK_PERIOD_MS;
    loading_overlay_show(s_screen);

    if (!bg_worker_submit_fn(_history_fetch_job, &s_history_job, _history_job_fn_done, NULL)) {
        ESP_LOGW(TAG, "History fetch: background worker busy — try again shortly");
        s_history_fetch_in_flight = false;
        loading_overlay_hide();
        toast_show(s_screen, "Busy - try again", TOAST_ERROR);
        return;
    }
    s_history_poll_timer = lv_timer_create(_history_poll_cb, 150, NULL);
}

static void _history_load_more_event(lv_event_t *e) {
    (void)e;
    _history_start_fetch(false);
}

static void _history_refresh_event(lv_event_t *e) {
    (void)e;
    _history_start_fetch(true);
}

void trip_screen_remove_history_item(int trip_id) {
    if (!s_history_items) return;
    for (int i = 0; i < s_history_loaded_count; i++) {
        if ((int)s_history_items[i].id == trip_id) {
            for (int j = i; j < s_history_loaded_count - 1; j++) s_history_items[j] = s_history_items[j + 1];
            s_history_loaded_count--;
            if (s_history_total_count > 0) s_history_total_count--;
            _history_render();
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  MANUAL FETCH TAB — unchanged from the previous single-tab screen
// ═══════════════════════════════════════════════════════════════
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
//  List row tap → JSON viewer (shared by both tabs)
// ═══════════════════════════════════════════════════════════════
static void _row_click(lv_event_t *e) {
    int trip_id = (int)(intptr_t)lv_event_get_user_data(e);
    trip_json_viewer_show(trip_id);
}

// ═══════════════════════════════════════════════════════════════
//  Manual tab list (re)build
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
    s_history_items = (trip_history_item_t *)heap_caps_calloc(HISTORY_MAX_LOADED, sizeof(trip_history_item_t), MALLOC_CAP_SPIRAM);
    if (!s_history_items) {
        ESP_LOGE(TAG, "History list: PSRAM allocation failed (%u bytes) — History tab will show an error, Manual Fetch still works",
                 (unsigned)(HISTORY_MAX_LOADED * sizeof(trip_history_item_t)));
    }

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
    ui_label(hdr, "TRIPS", C_TEXT);
    lv_obj_align(lv_obj_get_child(hdr, 0), LV_ALIGN_CENTER, 0, 0);

    // ── Tabs: History | Manual Fetch (doc 188) ──
    s_tab_history_btn = lv_btn_create(scr);
    lv_obj_set_size(s_tab_history_btn, 156, 32);
    lv_obj_align(s_tab_history_btn, LV_ALIGN_TOP_LEFT, 4, 34);
    lv_obj_set_style_shadow_width(s_tab_history_btn, 0, 0);
    lv_obj_set_style_radius(s_tab_history_btn, 6, 0);
    lv_obj_t *tab_history_lbl = ui_label(s_tab_history_btn, "History", C_TEXT);
    lv_obj_center(tab_history_lbl);
    lv_obj_add_event_cb(s_tab_history_btn, _tab_history_event, LV_EVENT_CLICKED, NULL);

    s_tab_manual_btn = lv_btn_create(scr);
    lv_obj_set_size(s_tab_manual_btn, 156, 32);
    lv_obj_align(s_tab_manual_btn, LV_ALIGN_TOP_RIGHT, -4, 34);
    lv_obj_set_style_shadow_width(s_tab_manual_btn, 0, 0);
    lv_obj_set_style_radius(s_tab_manual_btn, 6, 0);
    lv_obj_t *tab_manual_lbl = ui_label(s_tab_manual_btn, "Manual Fetch", C_TEXT);
    lv_obj_center(tab_manual_lbl);
    lv_obj_add_event_cb(s_tab_manual_btn, _tab_manual_event, LV_EVENT_CLICKED, NULL);

    // Content area below header+tabs, above the nav bar.
    #define TRIP_CONTENT_TOP 70
    #define TRIP_CONTENT_H   (480 - TRIP_CONTENT_TOP - 52)

    // ── HISTORY tab ──
    s_history_cont = lv_obj_create(scr);
    lv_obj_set_size(s_history_cont, 320, TRIP_CONTENT_H);
    lv_obj_align(s_history_cont, LV_ALIGN_TOP_MID, 0, TRIP_CONTENT_TOP);
    lv_obj_set_style_bg_opa(s_history_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_history_cont, 0, 0);
    lv_obj_set_style_pad_all(s_history_cont, 0, 0);
    lv_obj_clear_flag(s_history_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hist_subhdr = lv_obj_create(s_history_cont);
    lv_obj_set_size(hist_subhdr, 320, 28);
    lv_obj_align(hist_subhdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(hist_subhdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hist_subhdr, 0, 0);
    lv_obj_set_style_pad_all(hist_subhdr, 4, 0);
    lv_obj_clear_flag(hist_subhdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *hist_title = ui_label(hist_subhdr, "Completed Trips", C_TEXT2);
    lv_obj_align(hist_title, LV_ALIGN_LEFT_MID, 4, 0);

    s_history_refresh_btn = lv_btn_create(hist_subhdr);
    lv_obj_set_size(s_history_refresh_btn, 30, 22);
    lv_obj_align(s_history_refresh_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(s_history_refresh_btn, C_BTN, 0);
    lv_obj_set_style_shadow_width(s_history_refresh_btn, 0, 0);
    lv_obj_set_style_radius(s_history_refresh_btn, 4, 0);
    lv_obj_t *refresh_lbl = ui_label(s_history_refresh_btn, LV_SYMBOL_REFRESH, C_TEXT);
    lv_obj_center(refresh_lbl);
    lv_obj_add_event_cb(s_history_refresh_btn, _history_refresh_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *history_list_cont = lv_obj_create(s_history_cont);
    lv_obj_set_size(history_list_cont, 320, TRIP_CONTENT_H - 28);
    lv_obj_align(history_list_cont, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_opa(history_list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(history_list_cont, 0, 0);
    lv_obj_set_style_pad_all(history_list_cont, 8, 0);
    lv_obj_set_style_pad_row(history_list_cont, 6, 0);
    lv_obj_set_flex_flow(history_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(history_list_cont,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(history_list_cont, LV_DIR_VER);
    lv_obj_add_flag(history_list_cont, LV_OBJ_FLAG_SCROLLABLE);
    s_history_list_cont = history_list_cont;

    // ── MANUAL FETCH tab ──
    s_manual_cont = lv_obj_create(scr);
    lv_obj_set_size(s_manual_cont, 320, TRIP_CONTENT_H);
    lv_obj_align(s_manual_cont, LV_ALIGN_TOP_MID, 0, TRIP_CONTENT_TOP);
    lv_obj_set_style_bg_opa(s_manual_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_manual_cont, 0, 0);
    lv_obj_set_style_pad_all(s_manual_cont, 0, 0);
    lv_obj_clear_flag(s_manual_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *entry_lbl = ui_label(s_manual_cont, "Enter Trip Number", C_TEXT2);
    lv_obj_align(entry_lbl, LV_ALIGN_TOP_LEFT, 10, 4);

    lv_obj_t *input_box = ui_card(s_manual_cont, 190, 40);
    lv_obj_align(input_box, LV_ALIGN_TOP_LEFT, 8, 24);
    lv_obj_add_flag(input_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(input_box, _input_box_click, LV_EVENT_CLICKED, NULL);
    s_input_lbl = ui_label(input_box, "Tap to enter", C_TEXT2);
    lv_obj_center(s_input_lbl);

    lv_obj_t *fetch_btn = lv_btn_create(s_manual_cont);
    lv_obj_set_size(fetch_btn, 92, 40);
    lv_obj_align(fetch_btn, LV_ALIGN_TOP_RIGHT, -8, 24);
    lv_obj_set_style_bg_color(fetch_btn, C_SUCCESS, 0);
    lv_obj_set_style_shadow_width(fetch_btn, 0, 0);
    lv_obj_set_style_radius(fetch_btn, 8, 0);
    lv_obj_t *fetch_lbl = ui_label(fetch_btn, "FETCH", C_BG);
    lv_obj_center(fetch_lbl);
    lv_obj_add_event_cb(fetch_btn, _fetch_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *manual_list_cont = lv_obj_create(s_manual_cont);
    lv_obj_set_size(manual_list_cont, 320, TRIP_CONTENT_H - 72);
    lv_obj_align(manual_list_cont, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_bg_opa(manual_list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(manual_list_cont, 0, 0);
    lv_obj_set_style_pad_all(manual_list_cont, 8, 0);
    lv_obj_set_style_pad_row(manual_list_cont, 6, 0);
    lv_obj_set_flex_flow(manual_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(manual_list_cont,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(manual_list_cont, LV_DIR_VER);
    lv_obj_add_flag(manual_list_cont, LV_OBJ_FLAG_SCROLLABLE);
    s_list_cont = manual_list_cont;

    ui_add_nav_bar(scr, SCREEN_TRIPS);
    s_screen = scr;

    trip_json_viewer_create();
    trip_screen_refresh_list();
    _history_render();   // "No completed trips found yet." until trip_screen_on_shown() does the first real fetch
    _apply_trip_tab_styles();

    return scr;
}

lv_obj_t *trip_screen_get_screen(void) {
    return s_screen;
}

void trip_screen_on_shown(void) {
    // Auto-load, matching Android's TripHistoryFragment11 reloading on
    // every fragment resume — this device's top-level screens are
    // created once and reused (lv_scr_load(), never re-created), so
    // this is the equivalent hook, called from ui_switch_screen()
    // whenever the Trips tab is opened (doc 188).
    if (s_active_tab == TRIP_TAB_HISTORY) {
        _history_start_fetch(true);
    }
}
