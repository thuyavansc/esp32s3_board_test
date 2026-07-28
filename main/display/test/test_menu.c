/**
 * test_menu.c — Test Menu: numbered list of test UIs
 *
 * TEST nav tab -> this list (instead of jumping straight into a UI).
 * Tapping a row loads that test's screen; each test screen's back
 * button calls test_menu_return() to come back here.
 *
 * To add a new test UI: create its module under main/test/ (own
 * _create()/_get_screen() pair, same pattern as test_pax_meter.c or
 * color_palette_ui.c), then add one line to s_tests[] below.
 */
#include <stdio.h>
#include "esp_log.h"
#include "lvgl.h"
#include "test_menu.h"
#include "test_pax_meter.h"
#include "color_palette/color_palette_ui.h"
#include "network/network_screen.h"
#include "sms/sms_screen.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui";

typedef struct {
    const char *name;
    lv_obj_t *(*get_screen)(void);
} test_entry_t;

static const test_entry_t s_tests[] = {
    { "PAX A920Pro Meter UI",  test_pax_meter_get_screen },
    { "Color Palette Viewer",  color_palette_ui_get_screen },
    { "Network (Cellular/Hotspot)", network_screen_get_screen },
    { "SMS (Inbox/Send)",           sms_screen_get_screen },
};
#define TEST_COUNT (sizeof(s_tests) / sizeof(s_tests[0]))

static lv_obj_t *s_menu_screen = NULL;

static void _row_click(lv_event_t *e) {
    unsigned idx = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Test Menu -> %s", s_tests[idx].name);
    lv_obj_t *target = s_tests[idx].get_screen();
    if (target) lv_scr_load(target);
}

lv_obj_t *test_menu_init(void) {
    // Create every registered test screen up front (same convention
    // as the 4 main screens being pre-created in ui_init()).
    test_pax_meter_create();
    color_palette_ui_create();
    network_screen_create();
    sms_screen_create();

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
    ui_label(hdr, "TEST MENU", C_TEXT);
    lv_obj_align(lv_obj_get_child(hdr, 0), LV_ALIGN_CENTER, 0, 0);

    // Count subtitle
    char count_buf[24];
    snprintf(count_buf, sizeof(count_buf), "%u test UIs available", (unsigned)TEST_COUNT);
    lv_obj_t *count_lbl = ui_label(scr, count_buf, C_TEXT2);
    lv_obj_align(count_lbl, LV_ALIGN_TOP_MID, 0, 40);

    // Scrollable list (same pattern as the Settings screen scroll fix)
    lv_obj_t *scroll_cont = lv_obj_create(scr);
    lv_obj_set_size(scroll_cont, 320, 372);
    lv_obj_align(scroll_cont, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(scroll_cont, 8, 0);
    lv_obj_set_style_pad_row(scroll_cont, 8, 0);
    lv_obj_set_flex_flow(scroll_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll_cont,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_VER);
    lv_obj_add_flag(scroll_cont, LV_OBJ_FLAG_SCROLLABLE);

    for (unsigned i = 0; i < TEST_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(scroll_cont);
        lv_obj_set_size(row, 296, 48);
        lv_obj_set_style_bg_color(row, C_CARD, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, _row_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        char num_buf[4];
        snprintf(num_buf, sizeof(num_buf), "%02u", i + 1);
        lv_obj_t *num_lbl = ui_label(row, num_buf, C_ACCENT2);
        lv_obj_set_style_text_font(num_lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(num_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *name_lbl = ui_label(row, s_tests[i].name, C_TEXT);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 34, 0);

        lv_obj_t *chevron = ui_label(row, LV_SYMBOL_RIGHT, C_TEXT2);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    ui_add_nav_bar(scr, SCREEN_TEST);
    s_menu_screen = scr;
    return scr;
}

void test_menu_return(void) {
    if (s_menu_screen) lv_scr_load(s_menu_screen);
}
