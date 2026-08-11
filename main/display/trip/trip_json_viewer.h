#pragma once
// ================================================================
// trip_json_viewer.h — trip detail view (doc 184 issues #9/#10/#11)
//
// Was a plain pretty-printed JSON dump; now a proper trip detail card
// matching Android's card_trip_history.xml layout: a collapsed summary
// (Trip #, time range, status, total fare) that expands to show
// pickup/dropoff addresses + times, distance, duration, and the full
// fare breakdown. A "View Raw JSON" toggle keeps the original
// pretty-printer available underneath — see trip_json_viewer.c's own
// header comment for why: the exact field names in a real
// GET Trips/{id} response were never captured against this pass, so
// field extraction is best-effort with fallback names, and the raw
// JSON is the ground truth if a field doesn't show up in the card.
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
