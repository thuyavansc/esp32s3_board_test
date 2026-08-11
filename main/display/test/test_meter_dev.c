/**
 * test_meter_dev.c — Meter Dev View (Test Features entry 01)
 *
 * See test_meter_dev.h for the doc 179 background. Top half is the
 * original SCREEN_DASHBOARD cards (speed/fare/distance/time), moved
 * here unchanged; bottom half is new — the breakdown/GPS-trust/frame/
 * sync detail that makes a bench test readable without scrolling
 * serial output (doc 179 §5).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "lvgl.h"
#include "test_meter_dev.h"
#include "test_menu.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "backend/taximeter/fare_calc.h"
#include "backend/taximeter/session_store.h"
#include "backend/gps/gps_client.h"

static const char *TAG = "ui";

static lv_obj_t *s_screen = NULL;

static lv_obj_t *s_lbl_speed    = NULL;
static lv_obj_t *s_lbl_fare     = NULL;
static lv_obj_t *s_lbl_distance = NULL;
static lv_obj_t *s_lbl_time     = NULL;

static lv_obj_t *s_lbl_gps      = NULL;   // "FIX yes  hdop 2.3  sats 8"
static lv_obj_t *s_lbl_trust    = NULL;   // "TRUST active"
static lv_obj_t *s_lbl_frame    = NULL;   // "FRAME #3 status=Hybrid dist=120m act=8s inact=0s"
static lv_obj_t *s_lbl_breakdown= NULL;   // flagfall/dist/time/inactive/extras/special, 2 lines
static lv_obj_t *s_lbl_sync     = NULL;   // local/server ids

static lv_timer_t *s_refresh_timer = NULL;

static void _back_event(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "Meter Dev View: back to Test Features");
    test_menu_return();
}

static const char *_frame_status_name(time_frame_status_t st) {
    switch (st) {
        case TIME_FRAME_STABLE:       return "Stable";
        case TIME_FRAME_GPS_INACTIVE: return "GpsInactive";
        case TIME_FRAME_HYBRID:       return "Hybrid";
        case TIME_FRAME_TIME:         return "Time";
        case TIME_FRAME_DISTANCE:     return "Distance";
        case TIME_FRAME_PAUSED:       return "Paused";
        default:                      return "?";
    }
}

void test_meter_dev_refresh(void) {
    if (!s_screen) return;

    fare_calc_snapshot_t snap;
    fare_calc_get_snapshot(&snap);
    char b[96];

    if (s_lbl_speed) {
        snprintf(b, sizeof(b), "%.1f", snap.speed_kmh);
        lv_label_set_text(s_lbl_speed, b);
    }
    if (s_lbl_fare) {
        snprintf(b, sizeof(b), "%.2f", snap.total_fare_cents / 100.0);
        lv_label_set_text(s_lbl_fare, b);
    }
    if (s_lbl_distance) {
        snprintf(b, sizeof(b), "%.3f", snap.distance_km);
        lv_label_set_text(s_lbl_distance, b);
    }
    if (s_lbl_time) {
        time_t n = 0; time(&n);
        struct tm t; localtime_r(&n, &t);
        snprintf(b, sizeof(b), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        lv_label_set_text(s_lbl_time, b);
    }

    if (s_lbl_gps) {
        const gps_data_t *g = gps_client_get_latest();
        if (g) {
            snprintf(b, sizeof(b), "GPS  fix=%s  hdop=%.1f  sats=%d",
                     g->has_fix ? "YES" : "NO", g->hdop, g->satellites);
        } else {
            strlcpy(b, "GPS  fix=NO  (no data)", sizeof(b));
        }
        lv_label_set_text(s_lbl_gps, b);
    }
    if (s_lbl_trust) {
        // doc 179 D6: this is deliberately the fare engine's internal
        // TRUST flag (fare_calc's gps_active), not the live-fix
        // indicator shown above — the two can legitimately disagree
        // (doc 170 §2.4) and that disagreement is exactly what a bench
        // test needs to see.
        snprintf(b, sizeof(b), "TRUST %s (billing basis)", snap.gps_active ? "ACTIVE" : "INACTIVE");
        lv_label_set_text(s_lbl_trust, b);
    }
    if (s_lbl_frame) {
        time_frame_t frames[4];
        int n = fare_calc_get_time_frames(frames, 4);
        if (n > 0) {
            const time_frame_t *f = &frames[n - 1];   // most recent (current, if running)
            snprintf(b, sizeof(b), "FRAME #%d  %s  dist=%.0fm act=%.0fs inact=%.0fs",
                     n, _frame_status_name(f->status), f->distance_m, f->gps_active_time_s, f->gps_inactive_time_s);
        } else {
            strlcpy(b, "FRAME --  (no trip running)", sizeof(b));
        }
        lv_label_set_text(s_lbl_frame, b);
    }
    if (s_lbl_breakdown) {
        snprintf(b, sizeof(b),
                 "flag %.2f  dist %.2f  time %.2f\ninact %.2f  extra %.2f  spec %.2f",
                 snap.flag_fall_cents / 100.0, snap.distance_fare_cents / 100.0, snap.time_fare_cents / 100.0,
                 snap.gps_inactive_fare_cents / 100.0, snap.extras_cents / 100.0, snap.special_fares_cents / 100.0);
        lv_label_set_text(s_lbl_breakdown, b);
    }
    if (s_lbl_sync) {
        int32_t local_id = session_store_get_active_local_trip_id();
        int64_t server_id = session_store_get_active_server_job_id();
        snprintf(b, sizeof(b), "local #%ld   server #%lld%s",
                 (long)local_id, (long long)server_id, server_id > 0 ? "" : " (not synced)");
        lv_label_set_text(s_lbl_sync, b);
    }
}

static void _refresh_timer_cb(lv_timer_t *timer) {
    (void)timer;
    test_meter_dev_refresh();
}

void test_meter_dev_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_back_header(scr, "METER DEV VIEW", _back_event);

    // ── Speed card ──
    lv_obj_t *spd_card = ui_card(scr, 280, 100);
    lv_obj_align(spd_card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_border_width(spd_card, 2, 0);
    lv_obj_set_style_border_color(spd_card, C_ACCENT2, 0);

    lv_obj_t *spd_lbl_u = ui_label(spd_card, "km/h", C_TEXT2);
    lv_obj_align(spd_lbl_u, LV_ALIGN_TOP_MID, 0, 2);

    s_lbl_speed = ui_label(spd_card, "0", C_TEXT);
    lv_obj_set_style_text_font(s_lbl_speed, &lv_font_montserrat_28, 0);
    lv_obj_align(s_lbl_speed, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t *spd_unit = ui_label(spd_card, "SPEED", C_TEXT2);
    lv_obj_align(spd_unit, LV_ALIGN_BOTTOM_MID, 0, -2);

    // ── Fare card ──
    lv_obj_t *fare_card = ui_card(scr, 280, 70);
    lv_obj_align(fare_card, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_border_width(fare_card, 1, 0);
    lv_obj_set_style_border_color(fare_card, C_SUCCESS, 0);

    lv_obj_t *fare_lbl = ui_label(fare_card, "FARE", C_TEXT2);
    lv_obj_align(fare_lbl, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *dollar = ui_label(fare_card, "$", C_WARN);
    lv_obj_align(dollar, LV_ALIGN_LEFT_MID, 4, 6);

    s_lbl_fare = ui_label(fare_card, "0.00", C_TEXT);
    lv_obj_set_style_text_font(s_lbl_fare, &lv_font_montserrat_28, 0);
    lv_obj_align(s_lbl_fare, LV_ALIGN_RIGHT_MID, -8, 6);

    // ── DIST / TIME cards ──
    lv_obj_t *d1 = ui_card(scr, 136, 60);
    lv_obj_align(d1, LV_ALIGN_TOP_LEFT, 8, 226);
    ui_label(d1, "DIST", C_TEXT2);
    lv_obj_align(lv_obj_get_child(d1, 0), LV_ALIGN_TOP_MID, 0, 2);
    s_lbl_distance = ui_label(d1, "0.000", C_TEXT);
    lv_obj_align(s_lbl_distance, LV_ALIGN_CENTER, 0, 4);
    ui_label(d1, "km", C_TEXT2);
    lv_obj_align(lv_obj_get_child(d1, 2), LV_ALIGN_BOTTOM_MID, 0, -2);

    lv_obj_t *d2 = ui_card(scr, 136, 60);
    lv_obj_align(d2, LV_ALIGN_TOP_RIGHT, -8, 226);
    ui_label(d2, "TIME", C_TEXT2);
    lv_obj_align(lv_obj_get_child(d2, 0), LV_ALIGN_TOP_MID, 0, 2);
    s_lbl_time = ui_label(d2, "--:--:--", C_TEXT);
    lv_obj_align(s_lbl_time, LV_ALIGN_CENTER, 0, 4);

    // ── Debug detail block (doc 179 §5 dev-view mock-up) ──
    lv_obj_t *dbg = ui_card(scr, 304, 168);
    lv_obj_align(dbg, LV_ALIGN_TOP_MID, 0, 294);
    lv_obj_set_style_border_width(dbg, 1, 0);
    lv_obj_set_style_border_color(dbg, C_DIVIDER, 0);
    lv_obj_clear_flag(dbg, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_gps = ui_label(dbg, "GPS  fix=NO", C_TEXT2);
    lv_obj_set_style_text_font(s_lbl_gps, &lv_font_montserrat_10, 0);
    lv_obj_align(s_lbl_gps, LV_ALIGN_TOP_LEFT, 0, 0);

    s_lbl_trust = ui_label(dbg, "TRUST --", C_TEXT2);
    lv_obj_set_style_text_font(s_lbl_trust, &lv_font_montserrat_10, 0);
    lv_obj_align(s_lbl_trust, LV_ALIGN_TOP_LEFT, 0, 16);

    s_lbl_frame = ui_label(dbg, "FRAME --", C_TEXT2);
    lv_obj_set_style_text_font(s_lbl_frame, &lv_font_montserrat_10, 0);
    lv_obj_align(s_lbl_frame, LV_ALIGN_TOP_LEFT, 0, 32);

    lv_obj_t *div = ui_label(dbg, "-- breakdown --", C_TEXT2);
    lv_obj_set_style_text_font(div, &lv_font_montserrat_10, 0);
    lv_obj_align(div, LV_ALIGN_TOP_LEFT, 0, 52);

    s_lbl_breakdown = ui_label(dbg, "flag --  dist --  time --", C_TEXT);
    lv_obj_set_style_text_font(s_lbl_breakdown, &lv_font_montserrat_10, 0);
    lv_obj_align(s_lbl_breakdown, LV_ALIGN_TOP_LEFT, 0, 68);

    lv_obj_t *div2 = ui_label(dbg, "-- sync --", C_TEXT2);
    lv_obj_set_style_text_font(div2, &lv_font_montserrat_10, 0);
    lv_obj_align(div2, LV_ALIGN_TOP_LEFT, 0, 104);

    s_lbl_sync = ui_label(dbg, "local --  server --", C_TEXT);
    lv_obj_set_style_text_font(s_lbl_sync, &lv_font_montserrat_10, 0);
    lv_obj_align(s_lbl_sync, LV_ALIGN_TOP_LEFT, 0, 120);

    s_screen = scr;
    test_meter_dev_refresh();
    s_refresh_timer = lv_timer_create(_refresh_timer_cb, 1000, NULL);
}

lv_obj_t *test_meter_dev_get_screen(void) {
    return s_screen;
}
