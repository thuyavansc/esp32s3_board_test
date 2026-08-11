#pragma once
// ================================================================
// trip_screen.h — Trips: two tabs (doc 188)
//   HISTORY      — auto-loads completed trips (POST Job/GetAllBySearch)
//                  the moment the screen is shown, no ID entry needed.
//   MANUAL FETCH — the original screen's behavior: numeric-keypad trip
//                  entry, an always-visible sorted list of stored
//                  trips, FETCH with a non-blocking loading overlay,
//                  and the just-fetched trip highlighted in the list.
// ================================================================
#include "lvgl.h"

// Create the screen once (called from ui_init()).
lv_obj_t *trip_screen_create(void);

// Returns the cached screen — used by trip_json_viewer.c's back button.
lv_obj_t *trip_screen_get_screen(void);

// Re-reads stored trips and redraws the MANUAL FETCH tab's list. Called
// after every manual fetch/delete, and once when the screen is created.
void trip_screen_refresh_list(void);

// Removes one item from the in-memory HISTORY tab list and redraws it
// (does not touch the server) — called after deleting a history-sourced
// trip's local cache from trip_json_viewer.c, so the row doesn't keep
// showing until the next full History refresh.
void trip_screen_remove_history_item(int trip_id);

// Called from ui_switch_screen() every time the Trips tab is opened —
// auto-loads the HISTORY tab's first page, matching Android's
// TripHistoryFragment11 reloading on every fragment resume (this
// project's top-level screens are created once and reused via
// lv_scr_load(), so this call is the equivalent "just became visible"
// hook). No-ops if the Manual Fetch tab is currently active.
void trip_screen_on_shown(void);
