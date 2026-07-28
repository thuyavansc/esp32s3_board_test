#pragma once
// ================================================================
// text_keypad.h — reusable alphanumeric entry keyboard (modal)
//
// Same "single modal instance, Enter commits / Cancel discards"
// contract as numeric_keypad.h, but for free-form text (WiFi SSID,
// hotspot password, later SMS phone number + message body — Phase 2)
// where a 4x4 numeric grid isn't enough. Built on LVGL's own
// lv_keyboard + lv_textarea widgets (confirmed compiled into this
// build: CONFIG_LV_USE_KEYBOARD/TEXTAREA/BTNMATRIX=y in
// sdkconfig.esp32s3_board) instead of a hand-rolled QWERTY grid.
// ================================================================
#include <stdbool.h>
#include "lvgl.h"

typedef void (*text_keypad_cb_t)(const char *value, void *user_data);

// Shows the keyboard on top of `screen`, pre-filled with `initial` (may
// be NULL/empty). `title` labels the popup (e.g. "New SSID"). If
// `password_mode` is true, the textarea masks input (bullets) — for
// hotspot password entry. Calls `on_enter(text, user_data)` only if the
// user taps the keyboard's own OK (checkmark) key; tapping Cancel (the
// header "X" or the keyboard's hide-icon) discards it and calls nothing.
void text_keypad_show(lv_obj_t *screen, const char *title, const char *initial,
                       bool password_mode, text_keypad_cb_t on_enter, void *user_data);
