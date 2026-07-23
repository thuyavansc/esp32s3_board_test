#pragma once
// ================================================================
// trip_screen.h — Trip History: numeric-keypad trip entry, an
// always-visible sorted list of stored trips (replaces the old
// "LIST STORED" button), FETCH with a non-blocking loading overlay,
// and the just-fetched/updated trip highlighted in the list.
// ================================================================
#include "lvgl.h"

// Create the screen once (called from ui_init()).
lv_obj_t *trip_screen_create(void);

// Returns the cached screen — used by trip_json_viewer.c's back button.
lv_obj_t *trip_screen_get_screen(void);

// Re-reads stored trips and redraws the list. Called after every
// fetch/delete, and once when the screen is first created.
void trip_screen_refresh_list(void);
