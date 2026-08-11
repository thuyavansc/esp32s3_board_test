#pragma once
// ================================================================
// test_meter_dev.h — Meter Dev View (Test Features entry 01)
//
// Doc 179 §1.4/§5/Phase 1c/Phase 5 (2026-08-06): this is the OLD
// SCREEN_DASHBOARD cards UI, moved out of the bottom nav (ui_main.c's
// _create_dashboard()) into Test Features now that the production
// METER screen (display/meter/meter_screen.h) carries the driver-
// facing layout. Extended with the breakdown/GPS-trust/frame/sync
// detail a bench test actually needs, per doc 179 §5's dev-view
// mock-up — this is what makes Phase 6 (calculation correctness)
// readable at a glance instead of scrolling serial output.
// ================================================================
#include "lvgl.h"

void test_meter_dev_create(void);
lv_obj_t *test_meter_dev_get_screen(void);

// Called every 1s from this screen's own lv_timer — safe even while
// this screen isn't the one on-screen.
void test_meter_dev_refresh(void);
