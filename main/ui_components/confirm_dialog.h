#pragma once
// ================================================================
// confirm_dialog.h — modal Yes/No confirmation popup (e.g. "Delete
// trip #12772?"). Themed the same as the rest of the app.
// ================================================================
#include "lvgl.h"

typedef void (*confirm_dialog_cb_t)(void *user_data);

// Shows a modal confirm popup on top of `screen`. `on_confirm` is called
// only if the user taps the confirm button; tapping Cancel just closes it.
void confirm_dialog_show(lv_obj_t *screen, const char *message,
                          confirm_dialog_cb_t on_confirm, void *user_data);
