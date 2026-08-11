/**
 * gps_info_screen.c — live GPS sensor data (doc 179 D6)
 *
 * See gps_info_screen.h. Read-only display of gps_client_get_latest()
 * plus which backend is currently active — the standing rule from doc
 * 180 §6.4 ("test GNSS outdoors, allow 90 seconds") makes a live
 * satellites/HDOP readout the fastest way to tell "no sky" from
 * "still acquiring" without reading serial logs.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lvgl.h"
#include "gps_info_screen.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "backend/gps/gps_client.h"

static const char *TAG = "ui";

static lv_obj_t *s_screen = NULL;
static gps_info_back_cb_t s_back_cb = NULL;

static lv_obj_t *s_lbl_source  = NULL;
static lv_obj_t *s_lbl_fix     = NULL;
static lv_obj_t *s_lbl_lat     = NULL;
static lv_obj_t *s_lbl_lon     = NULL;
static lv_obj_t *s_lbl_alt     = NULL;
static lv_obj_t *s_lbl_speed   = NULL;
static lv_obj_t *s_lbl_course  = NULL;
static lv_obj_t *s_lbl_hdop    = NULL;
static lv_obj_t *s_lbl_sats    = NULL;
static lv_obj_t *s_lbl_quality = NULL;

static lv_timer_t *s_refresh_timer = NULL;

static void _back_event(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "GPS Info: back");
    if (s_back_cb) s_back_cb();
}

void gps_info_screen_set_back_cb(gps_info_back_cb_t cb) {
    s_back_cb = cb;
}

static const char *_source_name(gps_source_t src) {
    switch (src) {
        case GPS_SRC_GNSS:   return "GNSS (A7670E)";
        case GPS_SRC_NEO6M:  return "NEO-6M";
        case GPS_SRC_INJECT: return "Injected (serial)";
        default:             return "?";
    }
}

void gps_info_screen_refresh(void) {
    if (!s_screen) return;

    if (s_lbl_source) lv_label_set_text(s_lbl_source, _source_name(gps_client_get_active_source()));

    const gps_data_t *g = gps_client_get_latest();
    char b[48];

    if (s_lbl_fix) {
        lv_label_set_text(s_lbl_fix, (g && g->has_fix) ? "FIX" : "NO FIX");
        lv_obj_set_style_text_color(s_lbl_fix, (g && g->has_fix) ? C_SUCCESS : C_ERROR, 0);
    }
    if (s_lbl_lat) {
        snprintf(b, sizeof(b), "%.6f", g ? g->lat : 0.0);
        lv_label_set_text(s_lbl_lat, b);
    }
    if (s_lbl_lon) {
        snprintf(b, sizeof(b), "%.6f", g ? g->lon : 0.0);
        lv_label_set_text(s_lbl_lon, b);
    }
    if (s_lbl_alt) {
        snprintf(b, sizeof(b), "%.1f m", g ? g->alt : 0.0);
        lv_label_set_text(s_lbl_alt, b);
    }
    if (s_lbl_speed) {
        snprintf(b, sizeof(b), "%.1f km/h", g ? g->speed : 0.0);
        lv_label_set_text(s_lbl_speed, b);
    }
    if (s_lbl_course) {
        snprintf(b, sizeof(b), "%.0f deg", g ? g->course : 0.0);
        lv_label_set_text(s_lbl_course, b);
    }
    if (s_lbl_hdop) {
        snprintf(b, sizeof(b), "%.1f", g ? g->hdop : 0.0);
        lv_label_set_text(s_lbl_hdop, b);
    }
    if (s_lbl_sats) {
        snprintf(b, sizeof(b), "%d", g ? g->satellites : 0);
        lv_label_set_text(s_lbl_sats, b);
    }
    if (s_lbl_quality) {
        int q = g ? g->fix_quality : 0;
        snprintf(b, sizeof(b), "%d (%s)", q, q == 0 ? "no fix" : q == 1 ? "GPS" : "DGPS");
        lv_label_set_text(s_lbl_quality, b);
    }
}

static void _refresh_timer_cb(lv_timer_t *timer) {
    (void)timer;
    gps_info_screen_refresh();
}

static lv_obj_t *_row(lv_obj_t *parent, int y, const char *name, lv_obj_t **out_val) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 300, 34);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t *n = ui_label(card, name, C_TEXT2);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *v = ui_label(card, "--", C_TEXT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -4, 0);
    if (out_val) *out_val = v;
    return card;
}

void gps_info_screen_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_back_header(scr, "GPS INFO", _back_event);

    lv_obj_t *src_card = ui_card(scr, 304, 44);
    lv_obj_align(src_card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *src_title = ui_label(src_card, "Active source", C_TEXT2);
    lv_obj_align(src_title, LV_ALIGN_TOP_LEFT, 0, 0);
    s_lbl_source = ui_label(src_card, "--", C_TEXT);
    lv_obj_align(s_lbl_source, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    s_lbl_fix = ui_label(src_card, "NO FIX", C_ERROR);
    lv_obj_align(s_lbl_fix, LV_ALIGN_RIGHT_MID, 0, 0);

    int y = 92;
    _row(scr, y, "Latitude",    &s_lbl_lat);    y += 40;
    _row(scr, y, "Longitude",   &s_lbl_lon);    y += 40;
    _row(scr, y, "Altitude",    &s_lbl_alt);    y += 40;
    _row(scr, y, "Speed",       &s_lbl_speed);  y += 40;
    _row(scr, y, "Course",      &s_lbl_course); y += 40;
    _row(scr, y, "HDOP",        &s_lbl_hdop);   y += 40;
    _row(scr, y, "Satellites",  &s_lbl_sats);   y += 40;
    _row(scr, y, "Fix quality", &s_lbl_quality);

    s_screen = scr;
    gps_info_screen_refresh();
    s_refresh_timer = lv_timer_create(_refresh_timer_cb, 500, NULL);
}

lv_obj_t *gps_info_screen_get_screen(void) {
    return s_screen;
}
