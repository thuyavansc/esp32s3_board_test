/**
 * ui_widgets.c — Shared LVGL screen-building helpers
 *
 * Extracted from ui_main.c so screens under test/ can reuse the same
 * card/label/nav-bar/back-header building blocks instead of each
 * file re-implementing them.
 */
#include "esp_log.h"
#include "ui_widgets.h"
#include "ui_theme.h"

static const char *TAG = "ui";

// ═══════════════════════════════════════════════════════════════
//  Card / label helpers
// ═══════════════════════════════════════════════════════════════
lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ui_test_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, C_TEST_CARD, 0);
    lv_obj_set_style_border_color(c, C_DIVIDER, 0);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, lv_color_t col) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

void ui_click_event(lv_event_t *e) {
    const char *label = (const char *)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Click: %s", label);
    printf("[UI] %s\n", label);
}

// ═══════════════════════════════════════════════════════════════
//  Nav Bar — 4 buttons: DASH|TRIP|SET|TEST
//  78px each × 4 = 312px + gaps (fits 320) — was 5 buttons (incl. GPS)
//  in the source project this was ported from; this project has no
//  GPS/socket hardware or code, so that tab was dropped, not disabled.
//  Created on EVERY top-level screen.
// ═══════════════════════════════════════════════════════════════
static void _nav_btn_event(lv_event_t *e) {
    ui_screen_t s = (ui_screen_t)(uintptr_t)lv_event_get_user_data(e);
    const char *names[] = {"DASHBOARD", "TRIPS", "SETTINGS", "TEST"};
    ESP_LOGI(TAG, "NAV -> %s (%d)", names[s], (int)s);
    ui_switch_screen(s);
}

void ui_add_nav_bar(lv_obj_t *scr, ui_screen_t active) {
    lv_obj_t *nav = lv_obj_create(scr);
    lv_obj_set_size(nav, 320, 52);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav, C_NAV_BG, 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_pad_all(nav, 2, 0);
    lv_obj_set_style_pad_column(nav, 2, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

    const char *names[] = {"DASH", "TRIP", "SET", "TEST"};
    const int BW = 78;   // button width (78×4=312, + 3×2 gaps=6 → 318, fits 320)
    for (int i = 0; i < SCREEN_COUNT; i++) {
        bool is_active = (i == (int)active);
        lv_obj_t *btn = lv_btn_create(nav);
        lv_obj_set_size(btn, BW, 46);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, i * (BW + 2), 0);
        lv_obj_set_style_bg_color(btn,
            is_active ? C_NAV_ACT : C_BTN, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 5, 0);

        lv_obj_t *lbl = ui_label(btn, names[i], is_active ? C_BG : C_TEXT);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, _nav_btn_event,
            LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

// ═══════════════════════════════════════════════════════════════
//  Back header — used by every drill-down screen (test/* screens)
// ═══════════════════════════════════════════════════════════════
lv_obj_t *ui_back_header(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb) {
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 320, 36);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_ACCENT, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_btn_create(hdr);
    lv_obj_set_size(back_btn, 56, 28);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, C_BTN, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 6, 0);
    lv_obj_t *back_lbl = ui_label(back_btn, LV_SYMBOL_LEFT " Back", C_TEXT);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title_lbl = ui_label(hdr, title, C_TEXT);
    lv_obj_align(title_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

    return hdr;
}
