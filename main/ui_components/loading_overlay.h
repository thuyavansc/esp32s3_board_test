#pragma once
// ================================================================
// loading_overlay.h — small on-top spinner, doesn't hide the rest
// of the UI (unlike a full-screen blocking modal).
// ================================================================
#include "lvgl.h"

// Shows a small spinner panel on top of the given screen. Safe to call
// again while already showing (no-op if one is already up).
void loading_overlay_show(lv_obj_t *screen);

// Removes the overlay if present. Safe to call when not showing.
void loading_overlay_hide(void);
