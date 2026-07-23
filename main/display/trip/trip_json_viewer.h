#pragma once
// ================================================================
// trip_json_viewer.h — pretty-printed, scrollable view of one
// stored trip's raw JSON, with a back button and a DELETE (with
// confirm) button.
//
// One shared screen whose content is swapped per trip (same pattern
// as the Color Palette Viewer's detail screen) — not a screen per trip.
// ================================================================
#include "lvgl.h"

// Create the screen once (called from trip_screen.c's create function).
void trip_json_viewer_create(void);

// Loads `trip_id`'s stored JSON, pretty-prints it, and navigates to the
// viewer screen.
void trip_json_viewer_show(int trip_id);
