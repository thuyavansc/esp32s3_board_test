/**
 * ui_main.c — TaxiMeter LVGL UI
 *
 * 4 top-level screens: Dashboard, Trips, Settings, Test Menu.
 * Color theme: Cerulean/Teal palette — see ui_theme.h.
 *
 * Ported from esp32_display_taxi_3, which also had a GPS screen —
 * dropped during the port (this project has no GPS/socket hardware
 * or code at all), not just disabled.
 *
 * The PAX A920Pro clone and the Color Palette Viewer used to live
 * directly under the TEST nav tab. They now live under test/ and
 * are reached by drilling into the Test Menu list (see test/test_menu.c).
 *
 * KEY FIXES (carried over from the original implementation):
 *   1. Settings scroll: uses lv_obj flex-column scroll container so the
 *      LIST scrolls, not the text inside cards.
 *   2. Brightness slider: calls display_set_backlight_pwm() for real PWM.
 *   3. 4-tab nav bar: DASH|TRIP|SET|TEST — see ui_widgets.c.
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
#include "test/test_menu.h"
#include "trip/trip_screen.h"

static const char *TAG = "ui";

// ── Screen storage ─────────────────────────────────────────────
static lv_obj_t    *s_screens[SCREEN_COUNT] = {NULL};
static ui_screen_t  s_current = SCREEN_DASHBOARD;

// ── Dashboard update labels ────────────────────────────────────
static lv_obj_t *s_lbl_speed    = NULL;
static lv_obj_t *s_lbl_fare     = NULL;
static lv_obj_t *s_lbl_distance = NULL;
static lv_obj_t *s_lbl_time     = NULL;
static lv_obj_t *s_lbl_status   = NULL;

// ── Settings sliders (for reading values on event) ────────────
static lv_obj_t *s_slider_brightness = NULL;
static lv_obj_t *s_lbl_brightness_val = NULL;
static lv_obj_t *s_lbl_contrast_val   = NULL;

// ── Forward declarations ────────────────────────────────────────
static void _click_event(lv_event_t *e);
static void _brightness_event(lv_event_t *e);
static void _contrast_event(lv_event_t *e);

// ═══════════════════════════════════════════════════════════════
//  Events
// ═══════════════════════════════════════════════════════════════
static void _click_event(lv_event_t *e) {
    const char *label = (const char *)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Click: %s", label);
    printf("[UI] %s\n", label);
    if (s_lbl_status) lv_label_set_text(s_lbl_status, label);
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
//  DASHBOARD Screen
// ═══════════════════════════════════════════════════════════════
static void _create_dashboard(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Status bar ──
    lv_obj_t *sb = lv_obj_create(scr);
    lv_obj_set_size(sb, 320, 32);
    lv_obj_align(sb, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(sb, C_ACCENT, 0);
    lv_obj_set_style_border_width(sb, 0, 0);
    lv_obj_set_style_radius(sb, 0, 0);
    lv_obj_set_style_pad_all(sb, 4, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    s_lbl_status = ui_label(sb, "TAXIMETER — READY", C_TEXT);
    lv_obj_center(s_lbl_status);

    // ── Speed card ──
    lv_obj_t *spd_card = ui_card(scr, 280, 115);
    lv_obj_align(spd_card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_border_width(spd_card, 2, 0);
    lv_obj_set_style_border_color(spd_card, C_ACCENT2, 0);

    lv_obj_t *spd_lbl_u = ui_label(spd_card, "km/h", C_TEXT2);
    lv_obj_align(spd_lbl_u, LV_ALIGN_TOP_MID, 0, 2);

    s_lbl_speed = ui_label(spd_card, "0", C_TEXT);
    lv_obj_set_style_text_font(s_lbl_speed, &lv_font_montserrat_28, 0);
    lv_obj_align(s_lbl_speed, LV_ALIGN_CENTER, 0, 8);

    lv_obj_t *spd_unit = ui_label(spd_card, "SPEED", C_TEXT2);
    lv_obj_align(spd_unit, LV_ALIGN_BOTTOM_MID, 0, -2);

    // ── Fare card ──
    lv_obj_t *fare_card = ui_card(scr, 280, 85);
    lv_obj_align(fare_card, LV_ALIGN_TOP_MID, 0, 165);
    lv_obj_set_style_border_width(fare_card, 1, 0);
    lv_obj_set_style_border_color(fare_card, C_SUCCESS, 0);

    lv_obj_t *fare_lbl = ui_label(fare_card, "FARE", C_TEXT2);
    lv_obj_align(fare_lbl, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *dollar = ui_label(fare_card, "$", C_WARN);
    lv_obj_align(dollar, LV_ALIGN_LEFT_MID, 4, 4);

    s_lbl_fare = ui_label(fare_card, "0.00", C_TEXT);
    lv_obj_set_style_text_font(s_lbl_fare, &lv_font_montserrat_28, 0);
    lv_obj_align(s_lbl_fare, LV_ALIGN_RIGHT_MID, -8, 4);

    // ── Bottom info cards (DIST / TIME / WIFI) ──
    int card_y = 262;
    int card_h = 72;

    lv_obj_t *d1 = ui_card(scr, 94, card_h);
    lv_obj_align(d1, LV_ALIGN_TOP_LEFT, 8, card_y);
    ui_label(d1, "DIST", C_TEXT2);
    lv_obj_align(lv_obj_get_child(d1, 0), LV_ALIGN_TOP_MID, 0, 2);
    s_lbl_distance = ui_label(d1, "0.00", C_TEXT);
    lv_obj_align(s_lbl_distance, LV_ALIGN_CENTER, 0, 4);
    ui_label(d1, "km", C_TEXT2);
    lv_obj_align(lv_obj_get_child(d1, 2), LV_ALIGN_BOTTOM_MID, 0, -2);

    lv_obj_t *d2 = ui_card(scr, 94, card_h);
    lv_obj_align(d2, LV_ALIGN_TOP_MID, 0, card_y);
    ui_label(d2, "TIME", C_TEXT2);
    lv_obj_align(lv_obj_get_child(d2, 0), LV_ALIGN_TOP_MID, 0, 2);
    s_lbl_time = ui_label(d2, "--:--", C_TEXT);
    lv_obj_align(s_lbl_time, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t *d3 = ui_card(scr, 94, card_h);
    lv_obj_align(d3, LV_ALIGN_TOP_RIGHT, -8, card_y);
    ui_label(d3, "WIFI", C_TEXT2);
    lv_obj_align(lv_obj_get_child(d3, 0), LV_ALIGN_TOP_MID, 0, 2);
    ui_label(d3, "OK", C_SUCCESS);
    lv_obj_align(lv_obj_get_child(d3, 1), LV_ALIGN_CENTER, 0, 4);

    ui_add_nav_bar(scr, SCREEN_DASHBOARD);
    s_screens[SCREEN_DASHBOARD] = scr;
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

    ui_add_nav_bar(scr, SCREEN_SETTINGS);
    s_screens[SCREEN_SETTINGS] = scr;
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════

esp_err_t ui_init(void) {
    ESP_LOGI(TAG, "══ UI INIT — TaxiMeter 320x480 ══");

    _create_dashboard();
    s_screens[SCREEN_TRIPS] = trip_screen_create();
    _create_settings();
    s_screens[SCREEN_TEST] = test_menu_init();

    lv_scr_load(s_screens[SCREEN_DASHBOARD]);
    s_current = SCREEN_DASHBOARD;

    ESP_LOGI(TAG, "UI — READY (4 screens | teal palette | scroll fix | test menu)");
    return ESP_OK;
}

void ui_switch_screen(ui_screen_t screen) {
    if (screen >= SCREEN_COUNT || !s_screens[screen]) return;
    lv_scr_load(s_screens[screen]);
    s_current = screen;
    ESP_LOGI(TAG, "Screen: %d", (int)screen);
}

void ui_update_dashboard(double speed, double distance, double fare) {
    char b[32];
    if (s_lbl_speed) {
        snprintf(b, sizeof(b), "%.0f", speed);
        lv_label_set_text(s_lbl_speed, b);
    }
    if (s_lbl_fare) {
        snprintf(b, sizeof(b), "%.2f", fare);
        lv_label_set_text(s_lbl_fare, b);
    }
    if (s_lbl_distance) {
        snprintf(b, sizeof(b), "%.2f", distance);
        lv_label_set_text(s_lbl_distance, b);
    }
    if (s_lbl_time) {
        time_t n = 0; time(&n);
        struct tm *t = localtime(&n);
        snprintf(b, sizeof(b), "%02d:%02d", t->tm_hour, t->tm_min);
        lv_label_set_text(s_lbl_time, b);
    }
}

void ui_log_event(const char *msg) {
    if (s_lbl_status) lv_label_set_text(s_lbl_status, msg);
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
