#pragma once
// ================================================================
// ui_widgets.h — Shared LVGL screen-building helpers
//
// Used by ui_main.c (Dashboard/GPS/Trips/Settings) and every
// test/* screen, so screen layout code stays consistent instead
// of each file re-implementing the same card/label/header patterns.
// ================================================================
#include "lvgl.h"
#include "ui_main.h"

// Styled card panel (rounded, C_CARD background, non-scrollable)
lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

// Styled card for the PAX-style test screens (bottom border only, square corners)
lv_obj_t *ui_test_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

// Label with a given text color
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, lv_color_t col);

// Generic "tap → serial log + optional status label" handler.
// user_data must be a `const char *` passed via lv_obj_add_event_cb.
void ui_click_event(lv_event_t *e);

// Bottom 3-tab nav bar (METER|TRIP|SET, doc 179 §5), added to every top-level screen.
void ui_add_nav_bar(lv_obj_t *scr, ui_screen_t active);

// Compact drill-down header: "<" back button (left) + title label.
// Used by every screen reached FROM Test Features (or GPS Info from
// Settings) instead of the main nav bar.
// Returns the header container in case the caller wants to align content below it.
lv_obj_t *ui_back_header(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb);
