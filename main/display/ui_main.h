#pragma once
// ================================================================
// ui_main.h — TaxiMeter LVGL UI Screen Manager
//
// Screens:
//   SCREEN_DASHBOARD — main fare/speed dashboard
//   SCREEN_TRIPS     — trip history: keypad entry, list, JSON viewer (see display/trip/trip_screen.h)
//   SCREEN_SETTINGS  — settings list (scrollable) + brightness/contrast
//   SCREEN_TEST      — Test Menu (list of test UIs; see test/test_menu.h)
//
// Ported from esp32_display_taxi_3, which also had a SCREEN_GPS (real
// GPS lat/lon/speed + SEND button) — dropped during the port, not
// disabled: this project has no GPS/socket hardware or code at all.
// ================================================================
#include "esp_err.h"

// Screen IDs
typedef enum {
    SCREEN_DASHBOARD = 0,
    SCREEN_TRIPS,
    SCREEN_SETTINGS,
    SCREEN_TEST,
    SCREEN_COUNT      // 4 screens total
} ui_screen_t;

// Initialize all UI screens and show dashboard
esp_err_t ui_init(void);

// Switch to a specific screen
void ui_switch_screen(ui_screen_t screen);

// Update dashboard values (called periodically from app_main)
void ui_update_dashboard(double speed, double distance, double fare);

// GPS values are now owned by gps_screen.c (see gps_screen_update()) —
// this module no longer touches the GPS screen directly.

// Show a toast/log message in the dashboard status bar
void ui_log_event(const char *msg);

// Get current screen index
ui_screen_t ui_get_current_screen(void);
