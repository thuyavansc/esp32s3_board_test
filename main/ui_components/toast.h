#pragma once
// ================================================================
// toast.h — Android-style toast notification
//
// A small rounded banner near the bottom of a screen that appears,
// stays for ~2.2s, then removes itself. Never blocks touch input
// on the rest of the screen.
// ================================================================
#include "lvgl.h"

typedef enum {
    TOAST_SUCCESS,
    TOAST_ERROR,
    TOAST_INFO,
} toast_type_t;

// Show a toast on the given screen. Safe to call repeatedly — each
// call creates its own independent toast + auto-dismiss timer.
void toast_show(lv_obj_t *screen, const char *msg, toast_type_t type);
