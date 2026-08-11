#pragma once
// ================================================================
// gps_info_screen.h — live GPS sensor data (doc 179 D6, 2026-08-06)
//
// Restores the GPS screen ui_main.c's own header comment said was
// "dropped during the port... this project has no GPS/socket hardware
// or code at all" — no longer true: this board has a real GNSS (A7670E)
// + optional NEO-6M source (backend/gps/gps_client.h). Read-only: lat/
// lon/alt/speed/course/hdop/satellites/fix state + which source is
// active, refreshed live.
//
// Reachable from TWO places (doc 179 D6 — "add that in Settings AND in
// Test Features"): the Settings screen's "GPS Info" row, and the Test
// Features list's own "GPS Info" entry. Since both need their own
// correct back-navigation target, the caller sets the back-destination
// right before loading this screen.
// ================================================================
#include "lvgl.h"

typedef void (*gps_info_back_cb_t)(void);

void gps_info_screen_create(void);
lv_obj_t *gps_info_screen_get_screen(void);

// Must be called (with the right target) immediately before
// lv_scr_load(gps_info_screen_get_screen()) — the back button calls
// whatever was set last.
void gps_info_screen_set_back_cb(gps_info_back_cb_t cb);

// Called every 500ms from this screen's own lv_timer.
void gps_info_screen_refresh(void);
