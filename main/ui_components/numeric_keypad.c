/**
 * numeric_keypad.c — reusable 4x4 numeric entry keypad
 *
 * Only one keypad can reasonably be open at a time (it's a modal), so
 * state is kept as a small set of statics rather than a malloc'd context
 * — simpler, and matches the same single-instance pattern loading_overlay.c
 * already uses.
 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "numeric_keypad.h"
#include "display/ui_theme.h"
#include "display/ui_widgets.h"

#define KEYPAD_BUF_CAP 32

// Row-major 4x4 layout. Index 3 = backspace, index 15 = enter — everything
// else is a literal single character appended to the buffer.
static const char *KEY_LABELS[16] = {
    "1", "2", "3", LV_SYMBOL_BACKSPACE,
    "4", "5", "6", "-",
    "7", "8", "9", "+",
    ".", "0", "#", LV_SYMBOL_OK,
};
#define KEY_BACKSPACE_IDX 3
#define KEY_ENTER_IDX     15

static lv_obj_t *s_backdrop = NULL;
static lv_obj_t *s_readout  = NULL;
static char s_buf[KEYPAD_BUF_CAP];
static numeric_keypad_cb_t s_on_enter   = NULL;
static void               *s_user_data  = NULL;

static void _update_readout(void) {
    if (s_readout) lv_label_set_text(s_readout, s_buf[0] ? s_buf : "-");
}

static void _close(void) {
    if (s_backdrop) {
        lv_obj_del(s_backdrop);
        s_backdrop = NULL;
        s_readout  = NULL;
    }
}

static void _cancel_event(lv_event_t *e) {
    _close();  // typed text discarded — callback is NOT called
}

static void _key_event(lv_event_t *e) {
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);

    if (idx == KEY_BACKSPACE_IDX) {
        size_t len = strlen(s_buf);
        if (len > 0) s_buf[len - 1] = '\0';
        _update_readout();
        return;
    }

    if (idx == KEY_ENTER_IDX) {
        numeric_keypad_cb_t cb = s_on_enter;
        void *user_data = s_user_data;
        char result[KEYPAD_BUF_CAP];
        strlcpy(result, s_buf, sizeof(result));
        _close();
        if (cb) cb(result, user_data);
        return;
    }

    size_t len = strlen(s_buf);
    if (len < KEYPAD_BUF_CAP - 1) {
        s_buf[len]     = KEY_LABELS[idx][0];
        s_buf[len + 1] = '\0';
    }
    _update_readout();
}

void numeric_keypad_show(lv_obj_t *screen, const char *initial,
                          numeric_keypad_cb_t on_enter, void *user_data) {
    if (!screen) return;
    _close();  // in case one was somehow already open

    strlcpy(s_buf, initial ? initial : "", sizeof(s_buf));
    s_on_enter  = on_enter;
    s_user_data = user_data;

    // Backdrop — catches taps outside the panel, closes without confirming
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
    s_backdrop = backdrop;

    lv_obj_t *panel = lv_obj_create(backdrop);
    lv_obj_set_size(panel, 300, 330);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(panel, C_CARD, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, C_DIVIDER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header: title + cancel ──
    lv_obj_t *title = ui_label(panel, "Enter Number", C_TEXT2);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 2);

    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 32, 28);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_set_style_bg_color(cancel_btn, C_ERROR, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_t *cancel_lbl = ui_label(cancel_btn, LV_SYMBOL_CLOSE, C_TEXT);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, _cancel_event, LV_EVENT_CLICKED, NULL);

    // ── Readout ──
    lv_obj_t *readout_bg = ui_card(panel, 280, 36);
    lv_obj_align(readout_bg, LV_ALIGN_TOP_MID, 0, 28);
    s_readout = ui_label(readout_bg, s_buf[0] ? s_buf : "-", C_TEXT);
    lv_obj_set_style_text_font(s_readout, &lv_font_montserrat_16, 0);
    lv_obj_align(s_readout, LV_ALIGN_RIGHT_MID, -4, 0);

    // ── 4x4 key grid ──
    const int GRID_TOP = 76;
    const int KEY_W = 66, KEY_H = 52, GAP = 6;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int idx = row * 4 + col;
            bool is_backspace = (idx == KEY_BACKSPACE_IDX);
            bool is_enter     = (idx == KEY_ENTER_IDX);

            lv_obj_t *key = lv_btn_create(panel);
            lv_obj_set_size(key, KEY_W, KEY_H);
            lv_obj_align(key, LV_ALIGN_TOP_LEFT,
                         col * (KEY_W + GAP), GRID_TOP + row * (KEY_H + GAP));
            lv_obj_set_style_bg_color(key,
                is_enter ? C_SUCCESS : is_backspace ? C_WARN : C_BTN, 0);
            lv_obj_set_style_shadow_width(key, 0, 0);
            lv_obj_set_style_radius(key, 8, 0);

            lv_obj_t *lbl = ui_label(key, KEY_LABELS[idx],
                is_enter ? C_BG : C_TEXT);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
            lv_obj_center(lbl);

            lv_obj_add_event_cb(key, _key_event, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
        }
    }
}
