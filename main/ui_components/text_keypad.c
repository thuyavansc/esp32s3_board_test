/**
 * text_keypad.c — reusable alphanumeric entry keyboard (modal)
 *
 * Single-instance statics, same convention as numeric_keypad.c/
 * loading_overlay.c (only one modal reasonably open at a time).
 *
 * Wires LVGL's own lv_keyboard to a one-line lv_textarea: the
 * keyboard's built-in OK (checkmark) key sends LV_EVENT_READY, and its
 * hide-icon key sends LV_EVENT_CANCEL — both delivered to the keyboard
 * object itself (confirmed by reading lv_keyboard.c's default event
 * handler: lv_event_send(obj, LV_EVENT_READY/CANCEL, NULL) where obj IS
 * the keyboard). A header "X" button gives a second, more obvious
 * cancel affordance matching numeric_keypad.c's UX.
 */
#include <string.h>
#include <stdint.h>
#include "text_keypad.h"
#include "display/ui_theme.h"
#include "display/ui_widgets.h"

#define TEXT_KEYPAD_BUF_CAP 161  // roomy enough for SSID/password (<=64) and Phase 2's SMS body (<=160)

static lv_obj_t *s_backdrop = NULL;
static lv_obj_t *s_ta       = NULL;
static text_keypad_cb_t s_on_enter  = NULL;
static void            *s_user_data = NULL;

static void _close(void) {
    if (s_backdrop) {
        lv_obj_del(s_backdrop);
        s_backdrop = NULL;
        s_ta       = NULL;
    }
}

static void _cancel_event(lv_event_t *e) {
    _close();  // typed text discarded — callback is NOT called
}

static void _ready_event(lv_event_t *e) {
    text_keypad_cb_t cb = s_on_enter;
    void *user_data = s_user_data;
    char result[TEXT_KEYPAD_BUF_CAP];
    strlcpy(result, s_ta ? lv_textarea_get_text(s_ta) : "", sizeof(result));
    _close();
    if (cb) cb(result, user_data);
}

void text_keypad_show(lv_obj_t *screen, const char *title, const char *initial,
                       bool password_mode, text_keypad_cb_t on_enter, void *user_data) {
    if (!screen) return;
    _close();  // in case one was somehow already open

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
    lv_obj_set_size(panel, 304, 316);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(panel, C_CARD, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, C_DIVIDER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header: title + cancel ──
    lv_obj_t *title_lbl = ui_label(panel, title ? title : "Enter Text", C_TEXT2);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 2, 2);

    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 32, 28);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_set_style_bg_color(cancel_btn, C_ERROR, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_t *cancel_lbl = ui_label(cancel_btn, LV_SYMBOL_CLOSE, C_TEXT);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, _cancel_event, LV_EVENT_CLICKED, NULL);

    // ── Text area ──
    lv_obj_t *ta = lv_textarea_create(panel);
    lv_obj_set_size(ta, 286, 36);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 30);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, password_mode);
    lv_textarea_set_max_length(ta, TEXT_KEYPAD_BUF_CAP - 1);
    lv_textarea_set_text(ta, initial ? initial : "");
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_set_style_bg_color(ta, C_BG2, 0);
    lv_obj_set_style_text_color(ta, C_TEXT, 0);
    lv_obj_set_style_border_color(ta, C_DIVIDER, 0);
    s_ta = ta;

    // ── Keyboard ──
    lv_obj_t *kb = lv_keyboard_create(panel);
    lv_obj_set_size(kb, 286, 210);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 74);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, _ready_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, _cancel_event, LV_EVENT_CANCEL, NULL);
}
