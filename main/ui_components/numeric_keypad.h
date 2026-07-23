#pragma once
// ================================================================
// numeric_keypad.h — reusable 4x4 numeric entry keypad
//
// Modal overlay on top of the current screen. Layout:
//   1  2  3  <-(backspace)
//   4  5  6  -
//   7  8  9  +
//   .  0  #  Enter
// Plus a small "X" cancel button in the popup's own header.
//
// Enter commits the typed text and calls the callback; Cancel (or the
// backdrop) discards it and calls nothing — matches "if cancel clicked
// that entered should not go."
// ================================================================
#include "lvgl.h"

typedef void (*numeric_keypad_cb_t)(const char *value, void *user_data);

// Shows the keypad on top of `screen`, pre-filled with `initial` (may be
// NULL/empty). Calls `on_enter(text, user_data)` only if the user taps Enter.
void numeric_keypad_show(lv_obj_t *screen, const char *initial,
                          numeric_keypad_cb_t on_enter, void *user_data);
