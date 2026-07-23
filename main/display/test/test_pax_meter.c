/**
 * test_pax_meter.c — PAX A920Pro Taxi Meter UI Clone (Test Menu entry)
 *
 * Reference: docs/TestFunctionalities/display/ui design-display sample2.jpeg
 *
 * Moved out of ui_main.c unchanged (visuals/behavior untouched — that's
 * a separate follow-up). Only the navigation changed: the bottom 5-tab
 * nav bar is gone (this is a Test Menu drill-down, not a top-level
 * screen). The existing "☰ Meter ···" header already occupies the top
 * 36px exactly like the reference image, so instead of stacking a
 * second back-header on top of it, a small "< Back" button (same
 * style as the Color Palette Viewer's back button) is inserted into
 * that same header row, next to the "···" dots.
 *
 * Layout (portrait 320×480):
 *   [0-35]   Header bar: "≡ Meter  [< Back] ···"
 *   [35-80]  Taxi ID row: "T9522  NOT FOR HIRE  [lamp]"
 *   [80-200] Main fare: "$  0.00 / METER READY"
 *   [200-290]Fees: "$ INCL.  0.00 FEES & EXTRAS"
 *   [290-360]Tariff: "00 / TARIFF"
 *   [360-415]Action buttons: [START TRIP] [END TRIP]
 *   [415-428]Footer: device ID + date
 */
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "lvgl.h"
#include "test_pax_meter.h"
#include "test_menu.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui";

static lv_obj_t *s_screen = NULL;

// ── Live labels ─────────────────────────────────────────────────
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_fare_lbl   = NULL;
static lv_obj_t *s_meter_lbl  = NULL;

static void _back_event(lv_event_t *e) {
    ESP_LOGI(TAG, "PAX Meter: back to Test Menu");
    test_menu_return();
}

static void _start_event(lv_event_t *e) {
    ESP_LOGI(TAG, "TEST: START TRIP pressed");
    if (s_status_lbl) lv_obj_set_style_text_color(s_status_lbl, C_SUCCESS, 0);
    if (s_status_lbl) lv_label_set_text(s_status_lbl, "ON TRIP");
    if (s_meter_lbl)  lv_label_set_text(s_meter_lbl, "RUNNING");
}

static void _end_event(lv_event_t *e) {
    ESP_LOGI(TAG, "TEST: END TRIP pressed");
    if (s_status_lbl) lv_obj_set_style_text_color(s_status_lbl, C_WARN, 0);
    if (s_status_lbl) lv_label_set_text(s_status_lbl, "NOT FOR HIRE");
    if (s_meter_lbl)  lv_label_set_text(s_meter_lbl, "METER READY");
    if (s_fare_lbl)   lv_label_set_text(s_fare_lbl, "0.00");
}

void test_pax_meter_create(void) {
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

    // Back button — same style/behavior as the "< Back" button used on the
    // Color Palette Viewer screens (ui_back_header), placed next to the "..." dots.
    lv_obj_t *back_btn = lv_btn_create(hdr);
    lv_obj_set_size(back_btn, 56, 26);
    lv_obj_align(back_btn, LV_ALIGN_RIGHT_MID, -30, 0);
    lv_obj_set_style_bg_color(back_btn, C_BTN, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 6, 0);
    lv_obj_t *back_lbl = ui_label(back_btn, LV_SYMBOL_LEFT " Back", C_TEXT);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_10, 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, _back_event, LV_EVENT_CLICKED, NULL);

    ui_label(hdr, "...", C_TEXT2);
    lv_obj_align(lv_obj_get_child(hdr, 2), LV_ALIGN_RIGHT_MID, 0, 0);

    // ── Taxi ID + Status row ────────────────────────────────────
    lv_obj_t *id_row = ui_test_card(scr, 320, 44);
    lv_obj_align(id_row, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(id_row, C_TEST_BG, 0);

    lv_obj_t *taxi_id_lbl = ui_label(id_row, "Taxi ID", C_TEXT2);
    lv_obj_set_style_text_font(taxi_id_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(taxi_id_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *taxi_id_val = ui_label(id_row, "T9522", C_TEXT);
    lv_obj_align(taxi_id_val, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *status_lbl_title = ui_label(id_row, "Status", C_TEXT2);
    lv_obj_set_style_text_font(status_lbl_title, &lv_font_montserrat_10, 0);
    lv_obj_align(status_lbl_title, LV_ALIGN_TOP_LEFT, 70, 0);

    s_status_lbl = ui_label(id_row, "NOT FOR HIRE", C_WARN);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_LEFT, 70, 0);

    lv_obj_t *lamp_btn = lv_btn_create(id_row);
    lv_obj_set_size(lamp_btn, 52, 26);
    lv_obj_align(lamp_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(lamp_btn, C_BTN, 0);
    lv_obj_set_style_shadow_width(lamp_btn, 0, 0);
    lv_obj_set_style_radius(lamp_btn, 13, 0);
    lv_obj_set_style_border_color(lamp_btn, C_ACCENT2, 0);
    lv_obj_set_style_border_width(lamp_btn, 1, 0);
    ui_label(lamp_btn, LV_SYMBOL_SETTINGS, C_TEXT2);
    lv_obj_center(lv_obj_get_child(lamp_btn, 0));
    lv_obj_add_event_cb(lamp_btn, ui_click_event, LV_EVENT_CLICKED,
        (void *)"Lamp toggle");

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

    lv_obj_t *fees_val = ui_label(fees_sect, "0.00", C_TEXT);
    lv_obj_set_style_text_font(fees_val, &lv_font_montserrat_28, 0);
    lv_obj_align(fees_val, LV_ALIGN_TOP_RIGHT, -8, 4);

    lv_obj_t *fees_lbl = ui_label(fees_sect, "FEES & EXTRAS", C_TEXT2);
    lv_obj_set_style_text_font(fees_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(fees_lbl, LV_ALIGN_BOTTOM_RIGHT, -8, 0);

    // ── Tariff section ──────────────────────────────────────────
    lv_obj_t *tariff_sect = ui_test_card(scr, 320, 68);
    lv_obj_align(tariff_sect, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_set_style_bg_color(tariff_sect, C_TEST_BG, 0);

    lv_obj_t *tariff_val = ui_label(tariff_sect, "00", C_TEXT);
    lv_obj_set_style_text_font(tariff_val, &lv_font_montserrat_28, 0);
    lv_obj_align(tariff_val, LV_ALIGN_TOP_LEFT, 0, 0);

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

    ui_label(footer, "00B25703", C_TEXT2);
    lv_obj_set_style_text_font(lv_obj_get_child(footer, 0), &lv_font_montserrat_10, 0);
    lv_obj_align(lv_obj_get_child(footer, 0), LV_ALIGN_LEFT_MID, 0, 0);

    time_t now = 0;
    struct tm ti = {0};
    time(&now);
    localtime_r(&now, &ti);
    char date_buf[40];
    snprintf(date_buf, sizeof(date_buf), "%02d/%02d/%04d",
             ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
    ui_label(footer, date_buf, C_TEXT2);
    lv_obj_set_style_text_font(lv_obj_get_child(footer, 1), &lv_font_montserrat_10, 0);
    lv_obj_align(lv_obj_get_child(footer, 1), LV_ALIGN_RIGHT_MID, 0, 0);

    s_screen = scr;
}

lv_obj_t *test_pax_meter_get_screen(void) {
    return s_screen;
}
