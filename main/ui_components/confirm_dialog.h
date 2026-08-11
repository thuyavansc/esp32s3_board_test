#pragma once
// ================================================================
// confirm_dialog.h — modal Yes/No confirmation popup (e.g. "Delete
// trip #12772?"). Themed the same as the rest of the app.
// ================================================================
#include "lvgl.h"

typedef void (*confirm_dialog_cb_t)(void *user_data);

// Shows a modal confirm popup on top of `screen`. `on_confirm` is called
// if the user taps Yes; `on_cancel` (may be NULL — most callers just
// want tapping No to silently close the dialog) is called if the user
// taps No. Both receive the same `user_data`. doc 184 §7.2 added
// `on_cancel` — the trip-restore prompt needs a REAL action on decline
// (finalize the trip, not just dismiss), which the original
// confirm-only contract couldn't express.
void confirm_dialog_show(lv_obj_t *screen, const char *message,
                          confirm_dialog_cb_t on_confirm, confirm_dialog_cb_t on_cancel,
                          void *user_data);
