/**
 * ui_main.c — TaxiMeter LVGL UI
 *
 * 3 top-level screens (doc 179 §5 restructure, 2026-08-06): Meter,
 * Trips, Settings. Color theme: Cerulean/Teal palette — see ui_theme.h.
 *
 * The production METER screen (display/meter/meter_screen.h) is the
 * former Test Menu "PAX A920Pro Meter UI" mock-up, promoted and wired
 * to the real fare_calc/trip_manager/duty_client backend. The old
 * cards-style dashboard that used to live here moved to Test Features
 * as the dev/diagnostic view (display/test/test_meter_dev.h). Test
 * Features itself (formerly the bottom-nav TEST tab, test/test_menu.h)
 * is now reached only via Settings -> "Test Features" — see
 * _create_settings() below.
 *
 * KEY FIXES (carried over from the original implementation):
 *   1. Settings scroll: uses lv_obj flex-column scroll container so the
 *      LIST scrolls, not the text inside cards.
 *   2. Brightness slider: calls display_set_backlight_pwm() for real PWM.
 *   3. 3-tab nav bar: METER|TRIP|SET — see ui_widgets.c.
 *
 * SCREEN SWITCHING: lv_scr_load() — NEVER use HIDDEN flags for top-level screens.
 * NAV BAR: Created on every top-level screen via ui_add_nav_bar().
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui_main.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "display_driver.h"
#include "config.h"
#include "meter/meter_screen.h"
#include "test/test_menu.h"
#include "gps/gps_info_screen.h"
#include "login/login_screen.h"
#include "trip/trip_screen.h"
#include "backend/taximeter/session_store.h"
#include "backend/taximeter/duty_client.h"
#include "backend/taximeter/auth_client.h"
#include "backend/taximeter/reference_data.h"
#include "backend/bg_worker.h"
#include "ui_components/confirm_dialog.h"
#include "ui_components/loading_overlay.h"
#include "ui_components/toast.h"

static const char *TAG = "ui";

// ── Screen storage ─────────────────────────────────────────────
static lv_obj_t    *s_screens[SCREEN_COUNT] = {NULL};
static ui_screen_t  s_current = SCREEN_METER;

// ── Settings sliders (for reading values on event) ────────────
static lv_obj_t *s_slider_brightness = NULL;
static lv_obj_t *s_lbl_brightness_val = NULL;
static lv_obj_t *s_lbl_contrast_val   = NULL;

// ── Settings duty row (doc 179 D4 "both" — a pill on the meter header
// PLUS a status/toggle row here) ───────────────────────────────
static lv_obj_t   *s_duty_row_lbl = NULL;
static lv_timer_t *s_duty_row_timer = NULL;

// ── Logout (doc 182 10.9) ──────────────────────────────────────
static volatile bool s_logout_done = false;
static lv_timer_t   *s_logout_timer = NULL;

// ── Settings tariff-type row (doc 184 issue #4 — Maxi/Sedan switch,
// matching Android's TariffTypeFragment: a simple tap-a-row list,
// applies immediately, no separate confirm — see that file's own
// setOnClickListener) ───────────────────────────────────────────
static lv_obj_t *s_tariff_type_row_lbl = NULL;
static lv_obj_t *s_tariff_picker_backdrop = NULL;

// ── Forward declarations ────────────────────────────────────────
static void _click_event(lv_event_t *e);
static void _brightness_event(lv_event_t *e);
static void _contrast_event(lv_event_t *e);
static void _test_features_event(lv_event_t *e);
static void _gps_info_from_settings_event(lv_event_t *e);
static void _back_to_settings_from_gps(void);
static void _duty_row_event(lv_event_t *e);
static void _tariff_type_row_event(lv_event_t *e);
static void _refresh_tariff_type_row(void);
static void _logout_row_event(lv_event_t *e);

// ═══════════════════════════════════════════════════════════════
//  Events
// ═══════════════════════════════════════════════════════════════
static void _click_event(lv_event_t *e) {
    const char *label = (const char *)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Click: %s", label);
    printf("[UI] %s\n", label);
}

// "Test Features" row (doc 179 D3) — drills into what used to be the
// bottom-nav Test tab.
static void _test_features_event(lv_event_t *e) {
    (void)e;
    test_menu_show();
}

// "GPS Info" row (doc 179 D6) — same screen the Test Features list also
// links to; its back button returns HERE (Settings), not the test list,
// since it was entered directly from Settings this time.
static void _back_to_settings_from_gps(void) {
    ui_switch_screen(SCREEN_SETTINGS);
}

static void _gps_info_from_settings_event(lv_event_t *e) {
    (void)e;
    if (!gps_info_screen_get_screen()) gps_info_screen_create();
    gps_info_screen_set_back_cb(_back_to_settings_from_gps);
    lv_scr_load(gps_info_screen_get_screen());
}

// Duty row (doc 179 D4 "both") — same on/off logic as meter_screen.c's
// header pill, duplicated here rather than shared, since the two live
// in different modules with no existing shared "duty widget" — small
// enough that a second copy is simpler than a new abstraction.
static void _refresh_duty_row(void) {
    if (!s_duty_row_lbl) return;
    bool on = (session_store_get_duty_status() == DUTY_STATUS_ON_DUTY);
    char b[32];
    snprintf(b, sizeof(b), "Duty: %s (tap to toggle)", on ? "ON" : "OFF");
    lv_label_set_text(s_duty_row_lbl, b);
    lv_obj_set_style_text_color(s_duty_row_lbl, on ? C_SUCCESS : C_TEXT, 0);
}

static void _duty_row_timer_cb(lv_timer_t *timer) {
    (void)timer;
    _refresh_duty_row();
    // Piggybacked here rather than a new timer — the tariff type can
    // change asynchronously too (reference_data_fetch_all() seeds a
    // default the first time it loads, right after login), so this
    // row needs the same "keep it honest" periodic refresh the duty
    // row already has, not just a refresh at screen-build time.
    _refresh_tariff_type_row();
}

static void _duty_row_event(lv_event_t *e) {
    (void)e;
    bool on = (session_store_get_duty_status() == DUTY_STATUS_ON_DUTY);
    if (on) duty_client_go_off_duty();
    else    duty_client_go_on_duty();
    _refresh_duty_row();
}

// ═══════════════════════════════════════════════════════════════
//  TARIFF TYPE PICKER (doc 184 issue #4 — "we need the vehicle type
//  change maxi and sedan in the setting"). Matches Android's
//  TariffTypeFragment111.kt exactly: a plain list of the types
//  reference_data actually has loaded, tap one -> applied immediately,
//  no separate confirm step (that file's own setOnClickListener does
//  the same: sets the type and navigates straight back, nothing else).
//  Only changes the DEFAULT for the NEXT trip — this project doesn't
//  implement mid-trip tariff switching (trip_manager.h's own header
//  already states that's out of scope), matching Android's own
//  TaxiMeter.updateTariffType() being a separate, unused-here code path.
// ═══════════════════════════════════════════════════════════════
static void _refresh_tariff_type_row(void) {
    if (!s_tariff_type_row_lbl) return;
    char current[16];
    char b[48];
    if (session_store_get_tariff_type(current, sizeof(current)) && current[0]) {
        snprintf(b, sizeof(b), "Tariff Type: %s (tap to change)", current);
    } else {
        snprintf(b, sizeof(b), "Tariff Type: (none set)");
    }
    lv_label_set_text(s_tariff_type_row_lbl, b);
}

static void _close_tariff_picker(void) {
    if (s_tariff_picker_backdrop) {
        lv_obj_del(s_tariff_picker_backdrop);
        s_tariff_picker_backdrop = NULL;
    }
}

static void _tariff_picker_cancel_event(lv_event_t *e) {
    (void)e;
    _close_tariff_picker();
}

static void _tariff_picker_apply_confirmed(void *user_data) {
    const char *type = (const char *)user_data;
    session_store_set_tariff_type(type);
    ESP_LOGI(TAG, "Tariff type set to '%s' (Settings)", type);
    _close_tariff_picker();
    _refresh_tariff_type_row();
    toast_show(s_screens[SCREEN_SETTINGS], "Tariff type updated", TOAST_SUCCESS);
}

// doc 188: the picker used to apply-and-close on every tap, including
// re-tapping the type already in use, with no indication of what was
// currently selected — your ask was "current sedan if we click sedan
// just close, if sedan if we click maxi then give a dialog confirm...
// if yes click only change and close". Same-type tap is now a silent
// close (nothing actually changes); a different-type tap asks first,
// matching a real settings change rather than Android's own
// tap-applies-immediately TariffTypeFragment (Android has no
// confirm step here — this project adds one deliberately, same
// reasoning as doc 183's start-trip confirm: a fare-affecting change
// deserves a second tap on this hardware's smaller/denser screen where
// a mis-tap is easier to make than on a phone's larger list rows).
static void _tariff_picker_pick_event(lv_event_t *e) {
    const char *type = (const char *)lv_event_get_user_data(e);

    char current[16];
    bool has_current = session_store_get_tariff_type(current, sizeof(current)) && current[0];
    if (has_current && strcmp(current, type) == 0) {
        _close_tariff_picker();
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Change tariff type to %s?", type);
    confirm_dialog_show(s_screens[SCREEN_SETTINGS], msg, _tariff_picker_apply_confirmed, NULL, (void *)type);
}

static void _tariff_type_row_event(lv_event_t *e) {
    (void)e;
    if (s_tariff_picker_backdrop) return;   // already open

    char types[8][16];
    int count = reference_data_get_tariff_types(types, 8);
    if (count == 0) {
        toast_show(s_screens[SCREEN_SETTINGS], "No tariff data loaded yet — try 'ref fetch'", TOAST_ERROR);
        return;
    }

    lv_obj_t *screen = s_screens[SCREEN_SETTINGS];
    lv_obj_t *backdrop = lv_obj_create(screen);
    lv_obj_set_size(backdrop, 320, 480);
    lv_obj_set_pos(backdrop, 0, 0);
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_60, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_radius(backdrop, 0, 0);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(backdrop);
    s_tariff_picker_backdrop = backdrop;

    lv_obj_t *panel = lv_obj_create(backdrop);
    lv_obj_set_size(panel, 260, 60 + count * 46);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, C_CARD, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, C_DIVIDER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = ui_label(panel, "Select Tariff Type", C_TEXT2);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 28, 26);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, C_ERROR, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_t *cancel_lbl = ui_label(cancel_btn, LV_SYMBOL_CLOSE, C_TEXT);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, _tariff_picker_cancel_event, LV_EVENT_CLICKED, NULL);

    // types[][] is a LOCAL array here — each row's click handler needs
    // its own STABLE string to reference after this function returns
    // (the panel/backdrop, and its buttons, outlive this call). Static
    // storage, capped at the same 8-type limit passed to
    // reference_data_get_tariff_types() above.
    static char s_picker_types[8][16];
    memcpy(s_picker_types, types, sizeof(s_picker_types));

    char current[16];
    bool has_current = session_store_get_tariff_type(current, sizeof(current)) && current[0];

    for (int i = 0; i < count; i++) {
        bool is_current = has_current && strcmp(current, s_picker_types[i]) == 0;

        lv_obj_t *row = lv_btn_create(panel);
        lv_obj_set_size(row, 232, 38);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 34 + i * 44);
        lv_obj_set_style_bg_color(row, C_BTN, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        if (is_current) {
            // doc 188: "not indicating what we currently selected that
            // need to show in green color" — border + label color, same
            // C_SUCCESS the rest of the app already uses for "active/OK".
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_color(row, C_SUCCESS, 0);
        }
        lv_obj_t *row_lbl = ui_label(row, s_picker_types[i], is_current ? C_SUCCESS : C_TEXT);
        lv_obj_center(row_lbl);
        lv_obj_add_event_cb(row, _tariff_picker_pick_event, LV_EVENT_CLICKED, s_picker_types[i]);
    }
}

// ═══════════════════════════════════════════════════════════════
//  LOGOUT (doc 182 10.9) — Settings, last row. "once anything not fine
//  user may logout and do" — a clean escape hatch from any bad state.
//  Goes off duty FIRST (matching Android's LogoutUseCase, which calls
//  goOffDuty() before the server logout call — doc 182 §6 Fix J: our
//  auth_client_logout() didn't do this, leaving a logged-out driver
//  on-duty server-side).
// ═══════════════════════════════════════════════════════════════
static bool _job_logout(void *arg) {
    (void)arg;
    if (session_store_get_duty_status() == DUTY_STATUS_ON_DUTY) {
        // Safe to call from here even though we're already running ON
        // bg_worker's task — it only queues one more small job behind
        // this one (bg_worker.c's xQueueSend never blocks, including the
        // worker task calling itself — same reasoning meter_screen.c's
        // start-trip job already relies on).
        duty_client_go_off_duty();
    }
    return auth_client_logout() == ESP_OK;   // always ESP_OK per its own contract — clears session_store either way
}

static void _job_logout_done(bool success, void *arg, void *user_data) {
    (void)success; (void)arg; (void)user_data;
    s_logout_done = true;
}

static void _logout_poll_cb(lv_timer_t *timer) {
    if (!s_logout_done) return;
    loading_overlay_hide();
    lv_timer_del(s_logout_timer);
    s_logout_timer = NULL;
    ESP_LOGI(TAG, "Logout complete -> login screen");
    lv_scr_load(login_screen_get_screen());
}

static void _logout_confirmed(void *user_data) {
    (void)user_data;
    loading_overlay_show(s_screens[SCREEN_SETTINGS]);
    s_logout_done = false;
    if (!bg_worker_submit_fn(_job_logout, NULL, _job_logout_done, NULL)) {
        loading_overlay_hide();
        return;
    }
    s_logout_timer = lv_timer_create(_logout_poll_cb, 150, NULL);
}

static void _logout_row_event(lv_event_t *e) {
    (void)e;
    confirm_dialog_show(s_screens[SCREEN_SETTINGS], "Logout?", _logout_confirmed, NULL, NULL);
}

static void _brightness_event(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    display_set_backlight_pwm((uint8_t)val);
    if (s_lbl_brightness_val) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%ld%%", val);
        lv_label_set_text(s_lbl_brightness_val, buf);
    }
    ESP_LOGI(TAG, "Brightness: %ld%%", val);
}

static void _contrast_event(lv_event_t *e) {
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    if (s_lbl_contrast_val) {
        char buf[12];
        snprintf(buf, sizeof(buf), "%ld%%", val);
        lv_label_set_text(s_lbl_contrast_val, buf);
    }
    ESP_LOGI(TAG, "Contrast (test only): %ld%%", val);
}

// ═══════════════════════════════════════════════════════════════
//  SETTINGS Screen
//
//  FIX: Settings items now live inside a scrollable flex-column
//  container, not positioned directly on the screen. This ensures
//  the LIST scrolls when swiping — not the text inside cards.
//
//  Layout:
//    scr (non-scrollable)
//    ├── Header (fixed, 32px top)
//    ├── Brightness row (fixed, always visible, 58px)
//    ├── Contrast row   (fixed, always visible, 58px)
//    └── scroll_cont (scrollable flex-column, remaining height)
//          ├── Info card × N (33px each, 6px gap)
// ═══════════════════════════════════════════════════════════════
static void _create_settings(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);  // screen itself NOT scrollable

    // ── Header ──
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 32);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_ACCENT, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    ui_label(hdr, "SETTINGS", C_TEXT);
    lv_obj_align(lv_obj_get_child(hdr, 0), LV_ALIGN_CENTER, 0, 0);

    // ── Brightness control (fixed, always visible) ──
    lv_obj_t *bl_row = ui_card(scr, 304, 54);
    lv_obj_align(bl_row, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_border_width(bl_row, 1, 0);
    lv_obj_set_style_border_color(bl_row, C_DIVIDER, 0);

    ui_label(bl_row, "Brightness", C_TEXT2);
    lv_obj_align(lv_obj_get_child(bl_row, 0), LV_ALIGN_LEFT_MID, 0, -10);

    s_lbl_brightness_val = ui_label(bl_row, "100%", C_TEXT);
    lv_obj_align(s_lbl_brightness_val, LV_ALIGN_RIGHT_MID, 0, -10);

    s_slider_brightness = lv_slider_create(bl_row);
    lv_obj_set_size(s_slider_brightness, 200, 8);
    lv_obj_align(s_slider_brightness, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_slider_set_range(s_slider_brightness, 0, 100);
    lv_slider_set_value(s_slider_brightness, 100, LV_ANIM_OFF);
    // Slider styling — teal theme
    lv_obj_set_style_bg_color(s_slider_brightness, C_BG2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_slider_brightness, C_SUCCESS, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_slider_brightness, C_TEXT, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_slider_brightness, 4, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_slider_brightness, _brightness_event, LV_EVENT_VALUE_CHANGED, NULL);

    // ── Contrast control (fixed, always visible) ──
    lv_obj_t *ct_row = ui_card(scr, 304, 54);
    lv_obj_align(ct_row, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_border_width(ct_row, 1, 0);
    lv_obj_set_style_border_color(ct_row, C_DIVIDER, 0);

    ui_label(ct_row, "Contrast (test)", C_TEXT2);
    lv_obj_align(lv_obj_get_child(ct_row, 0), LV_ALIGN_LEFT_MID, 0, -10);

    s_lbl_contrast_val = ui_label(ct_row, "100%", C_TEXT);
    lv_obj_align(s_lbl_contrast_val, LV_ALIGN_RIGHT_MID, 0, -10);

    lv_obj_t *ct_slider = lv_slider_create(ct_row);
    lv_obj_set_size(ct_slider, 200, 8);
    lv_obj_align(ct_slider, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_slider_set_range(ct_slider, 0, 100);
    lv_slider_set_value(ct_slider, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ct_slider, C_BG2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ct_slider, C_ACCENT2, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ct_slider, C_TEXT, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ct_slider, 4, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ct_slider, _contrast_event, LV_EVENT_VALUE_CHANGED, NULL);

    // ═══════════════════════════════════════════════════════════
    //  SCROLL FIX: scrollable container for the info list
    //
    //  Available height for scroll area:
    //    480 total
    //    - 32 (header)
    //    - 54 + 6 (brightness row + gap)
    //    - 54 + 6 (contrast row + gap)
    //    - 52 (nav bar)
    //    = 276px available
    //
    //  Container is set to LV_FLEX_FLOW_COLUMN so items stack
    //  vertically. The container is scrollable, the screen is NOT.
    // ═══════════════════════════════════════════════════════════
    lv_obj_t *scroll_cont = lv_obj_create(scr);
    lv_obj_set_size(scroll_cont, 320, 276);
    lv_obj_align(scroll_cont, LV_ALIGN_TOP_MID, 0, 156);  // below contrast row
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(scroll_cont, 4, 0);
    lv_obj_set_style_pad_row(scroll_cont, 5, 0);
    lv_obj_set_style_pad_left(scroll_cont, 8, 0);
    lv_obj_set_style_pad_right(scroll_cont, 8, 0);
    // Flex column — items stack top-to-bottom, container scrolls vertically
    lv_obj_set_flex_flow(scroll_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll_cont,
        LV_FLEX_ALIGN_START,   // main-axis: top-to-bottom
        LV_FLEX_ALIGN_START,   // cross-axis: left-aligned
        LV_FLEX_ALIGN_START);  // track: start
    // Allow vertical scroll, block horizontal
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_VER);
    lv_obj_add_flag(scroll_cont, LV_OBJ_FLAG_SCROLLABLE);

    // ── Navigable rows (doc 179 D3/D6) — "Test Features" (the old
    // bottom-nav Test tab, now reached only from here) and "GPS Info"
    // (live GPS sensor data — restored, doc 179 D6). Drawn with a
    // chevron and a dedicated click handler, ahead of the plain static
    // info rows below. ──
    {
        lv_obj_t *card = lv_obj_create(scroll_cont);
        lv_obj_set_size(card, 300, 34);
        lv_obj_set_style_bg_color(card, C_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *lbl = ui_label(card, "Test Features", C_TEXT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_t *chev = ui_label(card, LV_SYMBOL_RIGHT, C_TEXT2);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_add_event_cb(card, _test_features_event, LV_EVENT_CLICKED, NULL);
    }
    {
        lv_obj_t *card = lv_obj_create(scroll_cont);
        lv_obj_set_size(card, 300, 34);
        lv_obj_set_style_bg_color(card, C_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *lbl = ui_label(card, "GPS Info", C_TEXT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_t *chev = ui_label(card, LV_SYMBOL_RIGHT, C_TEXT2);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_add_event_cb(card, _gps_info_from_settings_event, LV_EVENT_CLICKED, NULL);
    }
    {
        // Duty status/toggle row (doc 179 D4 "both").
        lv_obj_t *card = lv_obj_create(scroll_cont);
        lv_obj_set_size(card, 300, 34);
        lv_obj_set_style_bg_color(card, C_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        s_duty_row_lbl = ui_label(card, "Duty: OFF (tap to toggle)", C_TEXT);
        lv_obj_align(s_duty_row_lbl, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_add_event_cb(card, _duty_row_event, LV_EVENT_CLICKED, NULL);
    }
    {
        // Tariff type (Maxi/Sedan/...) switch row (doc 184 issue #4).
        lv_obj_t *card = lv_obj_create(scroll_cont);
        lv_obj_set_size(card, 300, 34);
        lv_obj_set_style_bg_color(card, C_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        s_tariff_type_row_lbl = ui_label(card, "Tariff Type: (tap to change)", C_TEXT);
        lv_obj_align(s_tariff_type_row_lbl, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_add_event_cb(card, _tariff_type_row_event, LV_EVENT_CLICKED, NULL);
    }

    // Info setting items (added to scroll_cont, not scr)
    const char *items[] = {
        "WiFi: CONNECTED",
        "IP: 192.168.x.x",
        "PSRAM: 8MB (Octal)",
        "OTA: disabled (code present)",
        "Fare Rate: $2.50/km",
        "Flag Fall: $3.80",
        "Storage: SPIFFS (Trips API)",
        "Display: 3.5\" ST7796S",
        "Board: ESP32-S3 N16R8V",
        "ESP-IDF: 5.4 + LVGL 8.3",
    };
    for (int i = 0; i < 10; i++) {
        // Each item: full-width card inside the flex container
        lv_obj_t *card = lv_obj_create(scroll_cont);
        lv_obj_set_size(card, 300, 34);  // width relative to flex container
        lv_obj_set_style_bg_color(card, C_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);  // card NOT scrollable
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *item_lbl = ui_label(card, items[i], C_TEXT);
        lv_obj_align(item_lbl, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_add_event_cb(card, _click_event, LV_EVENT_CLICKED,
            (void *)items[i]);
    }
    {
        // Logout — LAST row (doc 182 10.9: "in the set menu at last add
        // logout... once anything not fine user may logout and do").
        lv_obj_t *card = lv_obj_create(scroll_cont);
        lv_obj_set_size(card, 300, 34);
        lv_obj_set_style_bg_color(card, C_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *lbl = ui_label(card, "Logout", C_ERROR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_add_event_cb(card, _logout_row_event, LV_EVENT_CLICKED, NULL);
    }

    ui_add_nav_bar(scr, SCREEN_SETTINGS);
    s_screens[SCREEN_SETTINGS] = scr;

    _refresh_duty_row();
    _refresh_tariff_type_row();
    s_duty_row_timer = lv_timer_create(_duty_row_timer_cb, 2000, NULL);
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════

esp_err_t ui_init(void) {
    ESP_LOGI(TAG, "══ UI INIT — TaxiMeter 320x480 ══");

    // Top-level screens (doc 179 §5) — eager, same as before: all 3 are
    // reachable every session via the nav bar, so lazy-creating them
    // would just delay the same allocation to first tap for no RAM win.
    meter_screen_create();
    s_screens[SCREEN_METER] = meter_screen_get_screen();
    s_screens[SCREEN_TRIPS] = trip_screen_create();
    _create_settings();   // sets s_screens[SCREEN_SETTINGS] itself, at the end of the function

    // Test Features (doc 179 D3) — only the LIST screen is built here
    // (cheap); every sub-screen it links to is created lazily on first
    // tap (test_menu.c), not eagerly at boot (doc 179 §2/Phase 1d).
    test_menu_init();

    // Login screen (doc 179 §5/Phase 2) — shown FIRST, unless a
    // remembered session is still valid (doc 179 D2c), in which case
    // boot goes straight to the meter, same as Android skipping login
    // on a still-valid session.
    login_screen_create();
    s_current = SCREEN_METER;   // the tab the nav bar/ui_get_current_screen() reports once past login
    if (login_screen_can_auto_login()) {
        lv_scr_load(s_screens[SCREEN_METER]);
        ESP_LOGI(TAG, "UI INIT: remembered session still valid — skipping login screen");
    } else {
        lv_scr_load(login_screen_get_screen());
    }

    ESP_LOGI(TAG, "UI — READY (3 screens | teal palette | scroll fix | lazy test features | login gate)");
    return ESP_OK;
}

void ui_switch_screen(ui_screen_t screen) {
    if (screen >= SCREEN_COUNT || !s_screens[screen]) return;
    lv_scr_load(s_screens[screen]);
    s_current = screen;
    ESP_LOGI(TAG, "Screen: %d", (int)screen);
    // doc 188: Trips screen is created once and reused (never re-created),
    // so this is the "just became visible" hook the History tab needs to
    // auto-load, matching Android's TripHistoryFragment11 reloading on
    // every fragment resume.
    if (screen == SCREEN_TRIPS) trip_screen_on_shown();
}

void ui_log_event(const char *msg) {
    ESP_LOGI(TAG, "[event] %s", msg);
}

ui_screen_t ui_get_current_screen(void) { return s_current; }

// ═══════════════════════════════════════════════════════════════
//  LVGL MEMORY-POOL STATS (Phase 0 — doc 155 §12.4)
//
//  See ui_main.h for the full rationale and the threading split.
//  Short version: lv_mem_monitor() must only run on the LVGL thread,
//  so the refresh happens there and everyone else reads a plain cached
//  copy — no LVGL internals touched from the serial-command task.
// ═══════════════════════════════════════════════════════════════
static ui_lvgl_mem_stats_t s_lvgl_mem = {0};

void ui_refresh_lvgl_mem_stats(void) {
#if LV_MEM_CUSTOM == 0
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);

    s_lvgl_mem.total_bytes    = m.total_size;
    s_lvgl_mem.free_bytes     = m.free_size;
    s_lvgl_mem.free_biggest   = m.free_biggest_size;
    s_lvgl_mem.used_bytes     = m.total_size - m.free_size;
    s_lvgl_mem.used_pct       = m.used_pct;
    s_lvgl_mem.frag_pct       = m.frag_pct;

    // max_used is the number Phase 0 actually needs — the high-water
    // mark, not the instantaneous value. LVGL tracks it internally as
    // max_used; keep our own running peak too so this stays correct
    // even if a future LVGL version drops that field.
    if (m.max_used > s_lvgl_mem.max_used_bytes) {
        s_lvgl_mem.max_used_bytes = m.max_used;
    }
    if (s_lvgl_mem.used_bytes > s_lvgl_mem.max_used_bytes) {
        s_lvgl_mem.max_used_bytes = s_lvgl_mem.used_bytes;
    }
    s_lvgl_mem.valid = true;
#else
    // LV_MEM_CUSTOM=1 means LVGL uses plain malloc() instead of its own
    // pool — there is no pool to measure, and these numbers would be
    // meaningless rather than merely zero. Leave valid=false so "mem"
    // says so explicitly instead of printing a misleading 0KB.
    s_lvgl_mem.valid = false;
#endif
}

void ui_get_lvgl_mem_stats(ui_lvgl_mem_stats_t *out) {
    if (!out) return;
    *out = s_lvgl_mem;   // plain struct copy — no LVGL call, safe from any task
}
