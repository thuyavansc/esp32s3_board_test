#pragma once
// ================================================================
// meter_screen.h — the PRODUCTION taxi meter screen (SCREEN_METER)
//
// Doc 179 §5/Phase 1b/Phase 4 (2026-08-06): promoted out of the Test
// Menu, where it used to live as a visual mock-up ("01 PAX A920Pro
// Meter UI", test/test_pax_meter.c — START/END only changed label
// text, nothing was wired to the real meter). This is that same
// layout, now the production main screen and now genuinely live:
// every value comes from fare_calc_get_snapshot()/session_store/
// duty status, and START/END actually drive trip_manager.
//
// Reference: docs/TestFunctionalities/display/ui design-display sample2.jpeg
// Layout (portrait 320x480), unchanged from the original mock-up:
//   [0-35]   Header: "= Meter  [duty pill]  ..."
//   [35-80]  Taxi ID row: vehicle no + FOR HIRE/HIRED/OFF status
//   [80-200] Main fare: "$ 0.00 / METER READY"
//   [200-290]Fees: "$ INCL. 0.00 FEES & EXTRAS"
//   [290-360]Tariff row
//   [360-415]Action buttons: [START TRIP] [END TRIP]
//   [415-428]Footer: device id + date
// ================================================================
#include "lvgl.h"

void meter_screen_create(void);
lv_obj_t *meter_screen_get_screen(void);

// Called every 1s from this screen's own lv_timer (LVGL thread) — safe
// even while this screen isn't the one on-screen (just cheap label
// text-sets on an off-screen object).
void meter_screen_refresh(void);
