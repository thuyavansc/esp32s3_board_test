/**
 * meter_screen.c — the PRODUCTION taxi meter screen (SCREEN_METER)
 *
 * See meter_screen.h for the doc 179 §5/Phase 1b/Phase 4 background.
 * Visual layout is unchanged from the original test/test_pax_meter.c
 * mock-up (same PAX A920Pro reference) — everything below the surface
 * is new: every label is live, START/END drive the real trip_manager,
 * and a duty pill replaces the header's old "..." dots.
 *
 * THREADING: trip_manager_*()/duty_client_*() are all safe to call
 * directly from the LVGL thread — internally they only touch in-RAM
 * state synchronously and queue any HTTPS work onto bg_worker (see
 * trip_manager.h/duty_client.h) — same rule already relied on by
 * app_main.c's dashboard timer calling fare_calc_get_snapshot().
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "meter_screen.h"
#include "ui_main.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "config.h"
#include "backend/taximeter/fare_calc.h"
#include "backend/taximeter/trip_manager.h"
#include "backend/taximeter/session_store.h"
#include "backend/taximeter/duty_client.h"
#include "backend/taximeter/reference_data.h"
#include "backend/gps/gps_client.h"
#include "backend/network/net_manager.h"
#include "backend/bg_worker.h"
#include "ui_components/text_keypad.h"
#include "ui_components/confirm_dialog.h"
#include "ui_components/toast.h"
#include "ui_components/loading_overlay.h"

static const char *TAG = "ui";

static lv_obj_t *s_screen = NULL;

// ── Live labels ─────────────────────────────────────────────────
static lv_obj_t *s_duty_pill    = NULL;
static lv_obj_t *s_duty_pill_lbl= NULL;
static lv_obj_t *s_taxi_id_val  = NULL;
static lv_obj_t *s_status_lbl   = NULL;
static lv_obj_t *s_gps_net_lbl  = NULL;
static lv_obj_t *s_fare_lbl     = NULL;
static lv_obj_t *s_meter_lbl    = NULL;
static lv_obj_t *s_fees_val     = NULL;
static lv_obj_t *s_tariff_val   = NULL;
static lv_obj_t *s_footer_left  = NULL;

static lv_timer_t *s_refresh_timer = NULL;

// ═══════════════════════════════════════════════════════════════
//  DUTY PILL — doc 179 D4 ("both": a compact always-visible pill here,
//  PLUS a status row on the Settings screen, see settings_screen.c)
// ═══════════════════════════════════════════════════════════════
static void _apply_duty_pill_style(void) {
    if (!s_duty_pill || !s_duty_pill_lbl) return;
    bool on = (session_store_get_duty_status() == DUTY_STATUS_ON_DUTY);
    lv_obj_set_style_bg_color(s_duty_pill, on ? C_SUCCESS : C_BTN, 0);
    lv_label_set_text(s_duty_pill_lbl, on ? "ON DUTY" : "OFF DUTY");
}

static void _duty_off_confirmed(void *user_data) {
    (void)user_data;
    duty_client_go_off_duty();
    _apply_duty_pill_style();
    toast_show(s_screen, "Went OFF DUTY", TOAST_INFO);
}

static void _duty_pill_event(lv_event_t *e) {
    (void)e;
    bool on = (session_store_get_duty_status() == DUTY_STATUS_ON_DUTY);
    if (on) {
        // Turning OFF is the expensive mistake to make mid-shift —
        // confirm it (doc 179 Phase 3). Turning ON needs no confirm.
        confirm_dialog_show(s_screen, "Go OFF duty?", _duty_off_confirmed, NULL, NULL);
    } else {
        duty_client_go_on_duty();
        _apply_duty_pill_style();
        toast_show(s_screen, "Went ON DUTY", TOAST_SUCCESS);
    }
}

// ═══════════════════════════════════════════════════════════════
//  START TRIP — doc 182 Fix C/D/E: made async and self-healing.
//
//  Previously this called duty_client_go_on_duty() (fire-and-forget)
//  and then trip_manager_start_trip() on the SAME tick — a race, since
//  go_on_duty()'s reference-data refresh is 4 sequential HTTPS calls
//  that hadn't even started yet when start_trip() ran microseconds
//  later (doc 182 §1.5 Defect D). It also had no recovery if reference
//  data was empty for ANY reason (doc 182 §1.4's duty-persisted-ON
//  deadlock being the nastiest) — it just failed with a generic toast
//  (doc 182 §1.6 Defect E).
//
//  Now: the whole "make sure we CAN start" sequence — go on duty if
//  needed, ensure reference data is loaded (reference_data_ensure_
//  loaded(), doc 182 Fix C, which fetches synchronously if the arrays
//  are empty), then actually start — runs as ONE bg_worker job, with a
//  loading overlay and a specific error message per failure reason
//  (doc 182 Fix D/10.4).
// ═══════════════════════════════════════════════════════════════
typedef enum {
    START_ERR_NONE = 0,
    START_ERR_ALREADY_RUNNING,
    START_ERR_NO_REFDATA,
    START_ERR_NO_TARIFF,
    START_ERR_OTHER,
} start_trip_err_t;

static char s_pending_customer_name[32] = {0};
static volatile bool s_start_done = false;
static volatile bool s_start_success = false;
static volatile start_trip_err_t s_start_err = START_ERR_NONE;
static lv_timer_t *s_start_poll_timer = NULL;
static uint32_t    s_start_start_ms   = 0;
#define START_TRIP_WATCHDOG_MS 20000   // covers a from-scratch reference-data fetch (up to 4 sequential HTTPS calls)

static bool _job_prepare_and_start_trip(void *arg) {
    (void)arg;
    if (trip_manager_is_trip_active()) {
        s_start_err = START_ERR_ALREADY_RUNNING;
        return false;
    }
    if (session_store_get_duty_status() != DUTY_STATUS_ON_DUTY) {
        // Implicit go-on-duty on trip start (doc 179 Phase 3, matching
        // Android's PickUpFragment11.pickupAlert()). Safe to call from
        // here even though we're already running ON bg_worker's task —
        // it only queues two more small jobs behind this one (see
        // bg_worker.c: xQueueSend with a 0 timeout never blocks the
        // caller, including the worker task calling itself).
        duty_client_go_on_duty();
    }
    if (!reference_data_ensure_loaded()) {
        s_start_err = START_ERR_NO_REFDATA;
        return false;
    }
    esp_err_t err = trip_manager_start_trip(s_pending_customer_name[0] ? s_pending_customer_name : NULL);
    if (err == ESP_ERR_NOT_FOUND) { s_start_err = START_ERR_NO_TARIFF; return false; }
    if (err != ESP_OK)            { s_start_err = START_ERR_OTHER;     return false; }
    s_start_err = START_ERR_NONE;
    return true;
}

static void _job_prepare_and_start_trip_done(bool success, void *arg, void *user_data) {
    (void)arg; (void)user_data;
    s_start_success = success;
    s_start_done    = true;
}

static void _start_poll_cb(lv_timer_t *timer) {
    if (s_start_done) {
        loading_overlay_hide();
        if (s_start_success) {
            toast_show(s_screen, "Trip started", TOAST_SUCCESS);
        } else {
            // doc 182 10.4 — the SPECIFIC reason, not a generic "could
            // not start trip". A driver seeing "no tariff data" knows to
            // check signal; seeing "already running" knows to just wait.
            const char *msg;
            switch (s_start_err) {
                case START_ERR_ALREADY_RUNNING: msg = "Trip already running"; break;
                case START_ERR_NO_REFDATA:      msg = "No tariff data available - check network"; break;
                case START_ERR_NO_TARIFF:       msg = "No matching tariff for this vehicle type"; break;
                default:                        msg = "Could not start trip"; break;
            }
            toast_show(s_screen, msg, TOAST_ERROR);
        }
        meter_screen_refresh();
        lv_timer_del(s_start_poll_timer);
        s_start_poll_timer = NULL;
        return;
    }
    uint32_t elapsed_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_start_start_ms;
    if (elapsed_ms > START_TRIP_WATCHDOG_MS) {
        loading_overlay_hide();
        toast_show(s_screen, "Timed out - check network", TOAST_ERROR);
        lv_timer_del(s_start_poll_timer);
        s_start_poll_timer = NULL;
    }
}

// doc 179 D11(b): START opens a customer-name keypad first (optional —
// empty is accepted) rather than starting bare immediately.
static void _on_customer_name_entered(const char *value, void *user_data) {
    (void)user_data;
    if (s_start_poll_timer) return;   // a start is already in flight
    if (trip_manager_is_trip_active()) {
        toast_show(s_screen, "Trip already running", TOAST_ERROR);
        return;
    }
    strlcpy(s_pending_customer_name, value ? value : "", sizeof(s_pending_customer_name));

    loading_overlay_show(s_screen);
    s_start_done    = false;
    s_start_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (!bg_worker_submit_fn(_job_prepare_and_start_trip, NULL, _job_prepare_and_start_trip_done, NULL)) {
        loading_overlay_hide();
        toast_show(s_screen, "Busy - try again", TOAST_ERROR);
        return;
    }
    s_start_poll_timer = lv_timer_create(_start_poll_cb, 150, NULL);
}

// doc 184 issue #2 — "when click start trip need a dialog yes or no".
static void _start_confirmed(void *user_data) {
    (void)user_data;
    text_keypad_show(s_screen, "Customer Name (optional)", "", false,
                      _on_customer_name_entered, NULL);
}

static void _start_event(lv_event_t *e) {
    (void)e;
    if (trip_manager_is_trip_active()) {
        toast_show(s_screen, "Trip already running", TOAST_ERROR);
        return;
    }
    ESP_LOGI(TAG, "METER: START TRIP tapped");
    confirm_dialog_show(s_screen, "Start trip?", _start_confirmed, NULL, NULL);
}

static void _end_confirmed(void *user_data) {
    (void)user_data;
    trip_manager_finalize_trip();
    meter_screen_refresh();
}

static void _end_event(lv_event_t *e) {
    (void)e;
    if (!trip_manager_is_trip_active()) {
        toast_show(s_screen, "No trip is running", TOAST_ERROR);
        return;
    }
    ESP_LOGI(TAG, "METER: END TRIP tapped");
    confirm_dialog_show(s_screen, "End this trip and finalize?", _end_confirmed, NULL, NULL);
}

// ═══════════════════════════════════════════════════════════════
//  LIVE REFRESH — called every 1s from this screen's own lv_timer
// ═══════════════════════════════════════════════════════════════
void meter_screen_refresh(void) {
    if (!s_screen) return;

    fare_calc_snapshot_t snap;
    fare_calc_get_snapshot(&snap);

    // doc 184 issue #8 — "after end trip meter data still showing as it
    // is". fare_calc_stop() deliberately FREEZES totals (still readable
    // via fare_calc_get_snapshot() for the sync payload/trip history)
    // rather than clearing them, so the raw snapshot alone can't tell
    // "no trip yet" from "a trip just ended" apart. The display's job is
    // to show a genuinely idle meter once not running — matching
    // Android clearing its active-trip singletons on finalize (doc 149
    // §5) — not to keep echoing the last trip's numbers until a new one
    // starts. Only the live METER screen resets this way; the frozen
    // totals themselves are untouched and still reach trip history/sync
    // correctly.
    char b[40];
    if (s_fare_lbl) {
        snprintf(b, sizeof(b), "%.2f", snap.is_running ? snap.total_fare_cents / 100.0 : 0.0);
        lv_label_set_text(s_fare_lbl, b);
    }
    if (s_meter_lbl) {
        const char *txt = !snap.is_running ? "METER READY"
                         : snap.is_paused  ? "PAUSED"
                                            : "ON TRIP";
        lv_label_set_text(s_meter_lbl, txt);
    }
    if (s_status_lbl) {
        bool on_duty = (session_store_get_duty_status() == DUTY_STATUS_ON_DUTY);
        if (!on_duty) {
            lv_obj_set_style_text_color(s_status_lbl, C_TEXT2, 0);
            lv_label_set_text(s_status_lbl, "OFF DUTY");
        } else if (snap.is_running) {
            lv_obj_set_style_text_color(s_status_lbl, C_WARN, 0);
            lv_label_set_text(s_status_lbl, "HIRED");
        } else {
            lv_obj_set_style_text_color(s_status_lbl, C_SUCCESS, 0);
            lv_label_set_text(s_status_lbl, "FOR HIRE");
        }
    }
    if (s_fees_val) {
        double fees_cents = snap.is_running ? (snap.extras_cents + snap.special_fares_cents) : 0.0;
        snprintf(b, sizeof(b), "%.2f", fees_cents / 100.0);
        lv_label_set_text(s_fees_val, b);
    }
    if (s_tariff_val) {
        if (snap.is_running && snap.tariff_id > 0) snprintf(b, sizeof(b), "%lld", (long long)snap.tariff_id);
        else strlcpy(b, "--", sizeof(b));
        lv_label_set_text(s_tariff_val, b);
    }
    if (s_taxi_id_val) {
        char vno[16];
        if (session_store_get_vehicle_no(vno, sizeof(vno)) && vno[0]) {
            lv_label_set_text(s_taxi_id_val, vno);
        } else {
            lv_label_set_text(s_taxi_id_val, "--");
        }
    }
    if (s_gps_net_lbl) {
        // doc 179 D6(a): production shows a LIVE fix indicator, not the
        // fare engine's internal trust flag (that distinction — and why
        // it matters — lives on the dev meter view / GPS Info screen).
        const gps_data_t *g = gps_client_get_latest();
        bool has_fix = g && g->has_fix;
        net_uplink_t up = net_manager_get_active_uplink();
        const char *net_txt = up == NET_UPLINK_WIFI ? "WIFI" : up == NET_UPLINK_CELLULAR ? "CELL" : "OFF";
        snprintf(b, sizeof(b), "GPS %s\n%s", has_fix ? "OK" : "--", net_txt);
        lv_label_set_text(s_gps_net_lbl, b);
        lv_obj_set_style_text_color(s_gps_net_lbl, has_fix ? C_SUCCESS : C_TEXT2, 0);
    }
    if (s_footer_left) {
        int32_t local_id = session_store_get_active_local_trip_id();
        if (local_id > 0) snprintf(b, sizeof(b), "Trip #%ld", (long)local_id);
        else {
            char vno[16];
            session_store_get_vehicle_no(vno, sizeof(vno));
            strlcpy(b, vno[0] ? vno : "--", sizeof(b));
        }
        lv_label_set_text(s_footer_left, b);
    }

    _apply_duty_pill_style();
}

// doc 184 §7.2 — the trip-survives-reboot restore prompt. Checked once
// per boot, on this screen's own 1s refresh tick (which already runs
// regardless of which screen is currently visible — doc 181), gated on
// both "not shown yet" and "this screen is actually the one on
// screen" so the dialog doesn't try to appear behind the login screen
// before the driver has even logged in.
static bool s_restore_prompt_shown = false;

static void _restore_confirmed_yes(void *user_data) {
    (void)user_data;
    trip_manager_confirm_restore(true);
    toast_show(s_screen, "Trip resumed", TOAST_SUCCESS);
    meter_screen_refresh();
}

static void _restore_confirmed_no(void *user_data) {
    (void)user_data;
    trip_manager_confirm_restore(false);
    toast_show(s_screen, "Trip finalized", TOAST_INFO);
    meter_screen_refresh();
}

static void _refresh_timer_cb(lv_timer_t *timer) {
    (void)timer;
    meter_screen_refresh();

    if (!s_restore_prompt_shown && trip_manager_has_pending_restore() && ui_get_current_screen() == SCREEN_METER) {
        s_restore_prompt_shown = true;
        char summary[96];
        trip_manager_get_pending_restore_summary(summary, sizeof(summary));
        // doc 184 §7.2/D2 (your answer: "prompt the driver") — Yes
        // resumes billing from the carried-forward totals; No finalizes
        // immediately with whatever had accrued before the reboot, so
        // the fare isn't silently lost either way. Needed extending
        // confirm_dialog_show() with a real on_cancel callback (doc 184)
        // — its original contract only ever ran an action on confirm.
        confirm_dialog_show(s_screen, summary, _restore_confirmed_yes, _restore_confirmed_no, NULL);
    }
}

// ═══════════════════════════════════════════════════════════════
//  SCREEN CONSTRUCTION — layout unchanged from the original mock-up
// ═══════════════════════════════════════════════════════════════
void meter_screen_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_TEST_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header bar ─────────────────────────────────────────────
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 36);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_TEST_HDR, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 10, 0);
    lv_obj_set_style_pad_ver(hdr, 6, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *menu_lbl = ui_label(hdr, LV_SYMBOL_LIST " Meter", C_TEXT);
    lv_obj_align(menu_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    // Duty pill — replaces the old test-mock's "..." dots (doc 179 D4).
    s_duty_pill = lv_btn_create(hdr);
    lv_obj_set_size(s_duty_pill, 66, 26);
    lv_obj_align(s_duty_pill, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_shadow_width(s_duty_pill, 0, 0);
    lv_obj_set_style_border_width(s_duty_pill, 0, 0);
    lv_obj_set_style_radius(s_duty_pill, 13, 0);
    s_duty_pill_lbl = ui_label(s_duty_pill, "OFF DUTY", C_TEXT);
    lv_obj_set_style_text_font(s_duty_pill_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(s_duty_pill_lbl);
    lv_obj_add_event_cb(s_duty_pill, _duty_pill_event, LV_EVENT_CLICKED, NULL);

    // ── Taxi ID + Status row ────────────────────────────────────
    lv_obj_t *id_row = ui_test_card(scr, 320, 44);
    lv_obj_align(id_row, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(id_row, C_TEST_BG, 0);

    lv_obj_t *taxi_id_lbl = ui_label(id_row, "Taxi ID", C_TEXT2);
    lv_obj_set_style_text_font(taxi_id_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(taxi_id_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    s_taxi_id_val = ui_label(id_row, "--", C_TEXT);
    lv_obj_align(s_taxi_id_val, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *status_lbl_title = ui_label(id_row, "Status", C_TEXT2);
    lv_obj_set_style_text_font(status_lbl_title, &lv_font_montserrat_10, 0);
    lv_obj_align(status_lbl_title, LV_ALIGN_TOP_LEFT, 70, 0);

    s_status_lbl = ui_label(id_row, "OFF DUTY", C_TEXT2);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_LEFT, 70, 0);

    // GPS/NET mini indicator — doc 179 D6(a). Replaces the old mock's
    // decorative lamp-toggle gear button (never wired to anything real).
    s_gps_net_lbl = ui_label(id_row, "GPS --\nOFF", C_TEXT2);
    lv_obj_set_style_text_font(s_gps_net_lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(s_gps_net_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_gps_net_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    // ── Main fare section ───────────────────────────────────────
    lv_obj_t *fare_sect = ui_test_card(scr, 320, 120);
    lv_obj_align(fare_sect, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(fare_sect, C_TEST_BG, 0);

    ui_label(fare_sect, "$", C_TEXT);
    lv_obj_align(lv_obj_get_child(fare_sect, 0), LV_ALIGN_TOP_LEFT, 0, 0);

    s_fare_lbl = ui_label(fare_sect, "0.00", C_TEXT);
    lv_obj_set_style_text_font(s_fare_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(s_fare_lbl, LV_ALIGN_TOP_RIGHT, -8, 4);

    s_meter_lbl = ui_label(fare_sect, "METER READY", C_TEXT2);
    lv_obj_set_style_text_font(s_meter_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_meter_lbl, LV_ALIGN_BOTTOM_RIGHT, -8, 0);

    // ── Fees & Extras section ───────────────────────────────────
    lv_obj_t *fees_sect = ui_test_card(scr, 320, 90);
    lv_obj_align(fees_sect, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_set_style_bg_color(fees_sect, C_TEST_BG, 0);

    ui_label(fees_sect, "$", C_TEXT);
    lv_obj_align(lv_obj_get_child(fees_sect, 0), LV_ALIGN_TOP_LEFT, 0, 4);
    ui_label(fees_sect, "INCL.", C_TEXT2);
    lv_obj_set_style_text_font(lv_obj_get_child(fees_sect, 1), &lv_font_montserrat_10, 0);
    lv_obj_align(lv_obj_get_child(fees_sect, 1), LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_fees_val = ui_label(fees_sect, "0.00", C_TEXT);
    lv_obj_set_style_text_font(s_fees_val, &lv_font_montserrat_28, 0);
    lv_obj_align(s_fees_val, LV_ALIGN_TOP_RIGHT, -8, 4);

    lv_obj_t *fees_lbl = ui_label(fees_sect, "FEES & EXTRAS", C_TEXT2);
    lv_obj_set_style_text_font(fees_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(fees_lbl, LV_ALIGN_BOTTOM_RIGHT, -8, 0);

    // ── Tariff section ──────────────────────────────────────────
    lv_obj_t *tariff_sect = ui_test_card(scr, 320, 68);
    lv_obj_align(tariff_sect, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_set_style_bg_color(tariff_sect, C_TEST_BG, 0);

    s_tariff_val = ui_label(tariff_sect, "--", C_TEXT);
    lv_obj_set_style_text_font(s_tariff_val, &lv_font_montserrat_28, 0);
    lv_obj_align(s_tariff_val, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *tariff_lbl = ui_label(tariff_sect, "TARIFF", C_TEXT2);
    lv_obj_set_style_text_font(tariff_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(tariff_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // ── Action buttons: START TRIP / END TRIP ───────────────────
    lv_obj_t *start_btn = lv_btn_create(scr);
    lv_obj_set_size(start_btn, 156, 46);
    lv_obj_align(start_btn, LV_ALIGN_TOP_LEFT, 4, 362);
    lv_obj_set_style_bg_color(start_btn, C_SUCCESS, 0);
    lv_obj_set_style_shadow_width(start_btn, 0, 0);
    lv_obj_set_style_border_width(start_btn, 0, 0);
    lv_obj_set_style_radius(start_btn, 0, 0);
    ui_label(start_btn, "START TRIP", C_BG);
    lv_obj_set_style_text_font(lv_obj_get_child(start_btn, 0), &lv_font_montserrat_14, 0);
    lv_obj_center(lv_obj_get_child(start_btn, 0));
    lv_obj_add_event_cb(start_btn, _start_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *end_btn = lv_btn_create(scr);
    lv_obj_set_size(end_btn, 156, 46);
    lv_obj_align(end_btn, LV_ALIGN_TOP_RIGHT, -4, 362);
    lv_obj_set_style_bg_color(end_btn, C_BG2, 0);
    lv_obj_set_style_shadow_width(end_btn, 0, 0);
    lv_obj_set_style_border_color(end_btn, C_DIVIDER, 0);
    lv_obj_set_style_border_width(end_btn, 1, 0);
    lv_obj_set_style_radius(end_btn, 0, 0);
    ui_label(end_btn, "END TRIP", C_TEXT2);
    lv_obj_set_style_text_font(lv_obj_get_child(end_btn, 0), &lv_font_montserrat_14, 0);
    lv_obj_center(lv_obj_get_child(end_btn, 0));
    lv_obj_add_event_cb(end_btn, _end_event, LV_EVENT_CLICKED, NULL);

    // ── Footer ──────────────────────────────────────────────────
    lv_obj_t *footer = lv_obj_create(scr);
    lv_obj_set_size(footer, 320, 20);
    lv_obj_align(footer, LV_ALIGN_TOP_MID, 0, 408);
    lv_obj_set_style_bg_color(footer, C_TEST_BG, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_radius(footer, 0, 0);
    lv_obj_set_style_pad_hor(footer, 8, 0);
    lv_obj_set_style_pad_ver(footer, 2, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    s_footer_left = ui_label(footer, "--", C_TEXT2);
    lv_obj_set_style_text_font(s_footer_left, &lv_font_montserrat_10, 0);
    lv_obj_align(s_footer_left, LV_ALIGN_LEFT_MID, 0, 0);

    time_t now = 0;
    struct tm ti = {0};
    time(&now);
    localtime_r(&now, &ti);
    char date_buf[40];
    snprintf(date_buf, sizeof(date_buf), "%02d/%02d/%04d",
             ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
    lv_obj_t *date_lbl = ui_label(footer, date_buf, C_TEXT2);
    lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(date_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    // Nav bar — SCREEN_METER is a top-level tab (doc 179 §5).
    ui_add_nav_bar(scr, SCREEN_METER);

    s_screen = scr;
    meter_screen_refresh();
    s_refresh_timer = lv_timer_create(_refresh_timer_cb, 1000, NULL);
}

lv_obj_t *meter_screen_get_screen(void) {
    return s_screen;
}
