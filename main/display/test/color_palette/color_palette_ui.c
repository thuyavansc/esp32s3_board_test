/**
 * color_palette_ui.c — Color Palette Viewer (Test Menu entry)
 *
 * Grid screen: every named theme color (see color_palette_data.h) as
 * a swatch, 2 per row, scrollable — plus a color-wheel picker at the
 * bottom to test arbitrary colors against the physical panel (drag
 * the wheel, the preview box + hex readout below it update live).
 *
 * Detail screen: one shared screen whose content is swapped per tap
 * (not 19 separate screens) — a big swatch (~80% of the screen),
 * the color's name, and its hex code, with a back button to the grid.
 */
#include <stdio.h>
#include "esp_log.h"
#include "lvgl.h"
#include "color_palette_ui.h"
#include "color_palette_data.h"
#include "test_menu.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui";

static lv_obj_t *s_grid_screen   = NULL;
static lv_obj_t *s_detail_screen = NULL;

// ── Detail screen live elements ────────────────────────────────
static lv_obj_t *s_detail_header  = NULL;
static lv_obj_t *s_detail_title   = NULL;
static lv_obj_t *s_detail_swatch  = NULL;
static lv_obj_t *s_detail_name    = NULL;
static lv_obj_t *s_detail_hex     = NULL;

// ── Picker live elements ───────────────────────────────────────
static lv_obj_t *s_picker_preview = NULL;
static lv_obj_t *s_picker_hex_lbl = NULL;

static void _color_to_hex(lv_color_t color, char *out, size_t out_len) {
    lv_color32_t c32;
    c32.full = lv_color_to32(color);
    snprintf(out, out_len, "#%02X%02X%02X",
             LV_COLOR_GET_R32(c32), LV_COLOR_GET_G32(c32), LV_COLOR_GET_B32(c32));
}

static void _back_to_test_menu(lv_event_t *e) {
    test_menu_return();
}

static void _back_to_grid(lv_event_t *e) {
    lv_scr_load(s_grid_screen);
}

// ── Detail screen ───────────────────────────────────────────────
static void _build_detail_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_detail_header = ui_back_header(scr, "COLOR", _back_to_grid);
    s_detail_title = lv_obj_get_child(s_detail_header, 1);

    // Big swatch — ~80% of the screen area below the header
    s_detail_swatch = lv_obj_create(scr);
    lv_obj_set_size(s_detail_swatch, 300, 360);
    lv_obj_align(s_detail_swatch, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_border_width(s_detail_swatch, 1, 0);
    lv_obj_set_style_border_color(s_detail_swatch, C_DIVIDER, 0);
    lv_obj_set_style_radius(s_detail_swatch, 10, 0);
    lv_obj_clear_flag(s_detail_swatch, LV_OBJ_FLAG_SCROLLABLE);

    s_detail_name = ui_label(s_detail_swatch, "", C_TEXT);
    lv_obj_set_style_text_font(s_detail_name, &lv_font_montserrat_28, 0);
    lv_obj_align(s_detail_name, LV_ALIGN_CENTER, 0, -14);

    s_detail_hex = ui_label(s_detail_swatch, "", C_TEXT);
    lv_obj_set_style_text_font(s_detail_hex, &lv_font_montserrat_16, 0);
    lv_obj_align(s_detail_hex, LV_ALIGN_CENTER, 0, 20);

    s_detail_screen = scr;
}

static void _show_detail(int idx) {
    const palette_entry_t *e = &PALETTE_COLORS[idx];
    lv_color_t color = lv_color_hex(e->hex_value);
    lv_obj_set_style_bg_color(s_detail_swatch, color, 0);

    // Contrast-aware text: dark text on light swatches, light text on dark ones
    lv_color_t text_col = (lv_color_brightness(color) > 140) ? C_BG : C_TEXT;
    lv_obj_set_style_text_color(s_detail_name, text_col, 0);
    lv_obj_set_style_text_color(s_detail_hex, text_col, 0);

    lv_label_set_text(s_detail_name, e->name);
    lv_label_set_text(s_detail_hex, e->hex);
    lv_label_set_text(s_detail_title, e->name);

    ESP_LOGI(TAG, "Color detail: %s (%s / %s)", e->name, e->macro, e->hex);
    lv_scr_load(s_detail_screen);
}

static void _swatch_click(lv_event_t *e) {
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    _show_detail(idx);
}

// ── Colorwheel picker ───────────────────────────────────────────
static void _colorwheel_event(lv_event_t *e) {
    lv_obj_t *wheel = lv_event_get_target(e);
    lv_color_t picked = lv_colorwheel_get_rgb(wheel);
    lv_obj_set_style_bg_color(s_picker_preview, picked, 0);

    char hex[10];
    _color_to_hex(picked, hex, sizeof(hex));
    lv_label_set_text(s_picker_hex_lbl, hex);
}

// ── Grid screen ──────────────────────────────────────────────────
static void _build_grid_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_back_header(scr, "COLOR PALETTES", _back_to_test_menu);

    // Scrollable container: 320 x (480-36), holds the swatch grid + picker
    lv_obj_t *scroll_cont = lv_obj_create(scr);
    lv_obj_set_size(scroll_cont, 320, 444);
    lv_obj_align(scroll_cont, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(scroll_cont, 8, 0);
    lv_obj_set_style_pad_row(scroll_cont, 10, 0);
    lv_obj_set_flex_flow(scroll_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll_cont,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_VER);
    lv_obj_add_flag(scroll_cont, LV_OBJ_FLAG_SCROLLABLE);

    // "N colors" count label
    char count_buf[48];
    snprintf(count_buf, sizeof(count_buf), "%u colors to check (5 pure + theme)", (unsigned)PALETTE_COLOR_COUNT);
    ui_label(scroll_cont, count_buf, C_TEXT2);

    // ── Swatch grid: 2 per row, wraps to as many rows as needed ──
    lv_obj_t *grid_wrap = lv_obj_create(scroll_cont);
    lv_obj_set_width(grid_wrap, 296);
    lv_obj_set_height(grid_wrap, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid_wrap, 0, 0);
    lv_obj_set_style_pad_all(grid_wrap, 0, 0);
    lv_obj_set_style_pad_row(grid_wrap, 8, 0);
    lv_obj_set_style_pad_column(grid_wrap, 8, 0);
    lv_obj_clear_flag(grid_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid_wrap, LV_FLEX_FLOW_ROW_WRAP);

    for (unsigned i = 0; i < PALETTE_COLOR_COUNT; i++) {
        const palette_entry_t *e = &PALETTE_COLORS[i];

        lv_obj_t *card = lv_obj_create(grid_wrap);
        lv_obj_set_size(card, 140, 92);
        lv_obj_set_style_bg_color(card, C_BG2, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, _swatch_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *box = lv_obj_create(card);
        lv_obj_set_size(box, 130, 44);
        lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_set_style_bg_color(box, lv_color_hex(e->hex_value), 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, C_DIVIDER, 0);
        lv_obj_set_style_radius(box, 4, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name_lbl = ui_label(card, e->name, C_TEXT);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_10, 0);
        lv_obj_align(name_lbl, LV_ALIGN_BOTTOM_MID, 0, -14);

        lv_obj_t *hex_lbl = ui_label(card, e->hex, C_TEXT2);
        lv_obj_set_style_text_font(hex_lbl, &lv_font_montserrat_10, 0);
        lv_obj_align(hex_lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    // ── Color picker section ──────────────────────────────────────
    lv_obj_t *picker_lbl = ui_label(scroll_cont, "DRAG TO TEST A COLOR", C_TEXT2);
    lv_obj_set_style_text_font(picker_lbl, &lv_font_montserrat_14, 0);

    lv_obj_t *wheel = lv_colorwheel_create(scroll_cont, true);
    lv_obj_set_size(wheel, 180, 180);
    lv_obj_add_event_cb(wheel, _colorwheel_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *preview_row = lv_obj_create(scroll_cont);
    lv_obj_set_size(preview_row, 296, 60);
    lv_obj_set_style_bg_opa(preview_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(preview_row, 0, 0);
    lv_obj_set_style_pad_all(preview_row, 0, 0);
    lv_obj_clear_flag(preview_row, LV_OBJ_FLAG_SCROLLABLE);

    s_picker_preview = lv_obj_create(preview_row);
    lv_obj_set_size(s_picker_preview, 100, 50);
    lv_obj_align(s_picker_preview, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(s_picker_preview, 1, 0);
    lv_obj_set_style_border_color(s_picker_preview, C_DIVIDER, 0);
    lv_obj_set_style_radius(s_picker_preview, 6, 0);
    lv_obj_set_style_bg_color(s_picker_preview, lv_colorwheel_get_rgb(wheel), 0);
    lv_obj_clear_flag(s_picker_preview, LV_OBJ_FLAG_SCROLLABLE);

    s_picker_hex_lbl = ui_label(preview_row, "#------", C_TEXT);
    lv_obj_set_style_text_font(s_picker_hex_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_picker_hex_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
    {
        char hex[10];
        _color_to_hex(lv_colorwheel_get_rgb(wheel), hex, sizeof(hex));
        lv_label_set_text(s_picker_hex_lbl, hex);
    }

    s_grid_screen = scr;
}

void color_palette_ui_create(void) {
    _build_detail_screen();
    _build_grid_screen();
}

lv_obj_t *color_palette_ui_get_screen(void) {
    return s_grid_screen;
}
