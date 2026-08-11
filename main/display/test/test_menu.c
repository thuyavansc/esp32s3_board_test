/**
 * test_menu.c — Test Features: numbered list of dev/diagnostic UIs
 *
 * See test_menu.h. Doc 179 §5/D3 (2026-08-06): no longer a bottom-nav
 * tab — reached only from Settings -> "Test Features". Sub-screens are
 * now built LAZILY, on first tap (doc 179 §2/Phase 1d) — each entry's
 * create() only runs once get_screen() first returns NULL, instead of
 * every registered screen being built eagerly at boot regardless of
 * whether the driver ever opens Test Features in a given session.
 */
#include <stdio.h>
#include "esp_log.h"
#include "lvgl.h"
#include "test_menu.h"
#include "test_meter_dev.h"
#include "color_palette/color_palette_ui.h"
#include "network/network_screen.h"
#include "sms/sms_screen.h"
#include "gps/gps_info_screen.h"
#include "ui_main.h"
#include "ui_theme.h"
#include "ui_widgets.h"

static const char *TAG = "ui";

typedef struct {
    const char *name;
    void (*create)(void);
    lv_obj_t *(*get_screen)(void);
} test_entry_t;

static const test_entry_t s_tests[] = {
    { "Meter Dev View",             test_meter_dev_create,   test_meter_dev_get_screen },
    { "GPS Info",                   NULL /* see _row_click */, gps_info_screen_get_screen },
    { "Color Palette Viewer",       color_palette_ui_create, color_palette_ui_get_screen },
    { "Network (Cellular/Hotspot)", network_screen_create,   network_screen_get_screen },
    { "SMS (Inbox/Send)",           sms_screen_create,       sms_screen_get_screen },
};
#define TEST_COUNT (sizeof(s_tests) / sizeof(s_tests[0]))

static lv_obj_t *s_menu_screen = NULL;

static void _back_to_settings_event(lv_event_t *e) {
    (void)e;
    ui_switch_screen(SCREEN_SETTINGS);
}

static void _row_click(lv_event_t *e) {
    unsigned idx = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Test Features -> %s", s_tests[idx].name);

    // GPS Info needs its back-destination set before load (doc 179 D6 —
    // it's shared with Settings's own "GPS Info" row, each pointing
    // back to a different screen).
    if (s_tests[idx].get_screen == gps_info_screen_get_screen) {
        if (!gps_info_screen_get_screen()) gps_info_screen_create();
        gps_info_screen_set_back_cb(test_menu_return);
        lv_scr_load(gps_info_screen_get_screen());
        return;
    }

    if (!s_tests[idx].get_screen()) {
        // First visit this boot — build it now, not at startup.
        s_tests[idx].create();
    }
    lv_obj_t *target = s_tests[idx].get_screen();
    if (target) lv_scr_load(target);
}

void test_menu_init(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Back header, not the old top-level nav bar — Test Features is a
    // drill-down from Settings now, not a bottom tab (doc 179 D3/D9).
    ui_back_header(scr, "TEST FEATURES", _back_to_settings_event);

    // Count subtitle
    char count_buf[24];
    snprintf(count_buf, sizeof(count_buf), "%u test UIs available", (unsigned)TEST_COUNT);
    lv_obj_t *count_lbl = ui_label(scr, count_buf, C_TEXT2);
    lv_obj_align(count_lbl, LV_ALIGN_TOP_MID, 0, 44);

    // Scrollable list (same pattern as the Settings screen scroll fix)
    lv_obj_t *scroll_cont = lv_obj_create(scr);
    lv_obj_set_size(scroll_cont, 320, 390);
    lv_obj_align(scroll_cont, LV_ALIGN_TOP_MID, 0, 66);
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

    s_menu_screen = scr;
}

void test_menu_show(void) {
    if (s_menu_screen) lv_scr_load(s_menu_screen);
}

void test_menu_return(void) {
    if (s_menu_screen) lv_scr_load(s_menu_screen);
}
