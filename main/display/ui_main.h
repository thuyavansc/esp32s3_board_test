#pragma once
// ================================================================
// ui_main.h — TaxiMeter LVGL UI Screen Manager
//
// Screens (doc 179 §5 restructure, 2026-08-06):
//   SCREEN_METER    — the production meter: PAX A920Pro-style layout
//                      (display/meter/meter_screen.h), wired to the
//                      real fare_calc snapshot. Was the Test Menu's
//                      "01 PAX A920Pro Meter UI" entry — promoted to
//                      the production main screen; the mock-up no
//                      longer lives under Test.
//   SCREEN_TRIPS    — trip history: keypad entry, list, JSON viewer (see display/trip/trip_screen.h)
//   SCREEN_SETTINGS — settings list (scrollable) + brightness/contrast
//                      + a "Test Features" row that drills into what
//                      used to be the bottom-nav Test tab (test/
//                      test_menu.h) — no longer a top-level tab.
//
// Nav bar is now 3 tabs (METER | TRIP | SET), not 4 — the old "DASH"
// cards UI moved to Test Features as the dev/diagnostic meter view
// (display/test/test_meter_dev.h), and the Test tab itself moved
// behind Settings -> Test Features.
//
// Ported from esp32_display_taxi_3, which also had a SCREEN_GPS (real
// GPS lat/lon/speed + SEND button) — dropped during the original port
// as "no GPS hardware". That's no longer true (this board has a real
// GNSS/NEO-6M source, backend/gps/gps_client.h) — a GPS info screen is
// back, reachable from Settings and from Test Features (doc 179 D6).
// ================================================================
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Screen IDs — top-level, bottom-nav screens only.
typedef enum {
    SCREEN_METER = 0,
    SCREEN_TRIPS,
    SCREEN_SETTINGS,
    SCREEN_COUNT      // 3 screens total
} ui_screen_t;

// Initialize all UI screens and show the meter
esp_err_t ui_init(void);

// Switch to a specific screen
void ui_switch_screen(ui_screen_t screen);

// Live meter values are now owned by meter_screen.c/test_meter_dev.c
// directly (each reads fare_calc_get_snapshot() from its own 1s
// lv_timer) — this module no longer relays dashboard numbers.

// Show a toast/log message on the meter screen's status area.
void ui_log_event(const char *msg);

// Get current screen index
ui_screen_t ui_get_current_screen(void);

// ── LVGL memory-pool stats (Phase 0 — doc 155 §12.4) ────────────
//
// WHY THIS EXISTS: CONFIG_LV_MEM_SIZE_KILOBYTES reserves a fixed static
// array in INTERNAL SRAM (128KB before Phase 0) — the single biggest
// internal-SRAM consumer in this firmware. To size it correctly instead
// of guessing, we need to see how much of that pool the UI actually
// uses at peak.
//
// THREADING — this is the whole reason it's split into two functions:
// lv_mem_monitor() walks LVGL's internal allocator structures, so it
// must ONLY be called from the LVGL thread (doc 116's rule — never
// touch LVGL from another task). So:
//   ui_refresh_lvgl_mem_stats()  → called from the dashboard lv_timer
//                                   (LVGL thread) — does the real read
//   ui_get_lvgl_mem_stats()      → safe from ANY task (serial cmd,
//                                   bg_worker) — just copies out the
//                                   plain cached numbers
// diag.c's "mem" command uses the getter, so it never touches LVGL.
typedef struct {
    bool     valid;            // false until the lv_timer has run at least once
    uint32_t total_bytes;      // pool size (= CONFIG_LV_MEM_SIZE_KILOBYTES * 1024)
    uint32_t free_bytes;
    uint32_t free_biggest;     // largest single free block inside the pool
    uint32_t used_bytes;
    uint32_t max_used_bytes;   // PEAK usage since boot — the number that decides the safe pool size
    uint8_t  used_pct;
    uint8_t  frag_pct;
} ui_lvgl_mem_stats_t;

// Call ONLY from the LVGL thread (the dashboard lv_timer does this).
void ui_refresh_lvgl_mem_stats(void);

// Safe to call from any task — copies out the last cached snapshot.
void ui_get_lvgl_mem_stats(ui_lvgl_mem_stats_t *out);
