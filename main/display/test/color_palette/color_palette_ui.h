#pragma once
// ================================================================
// color_palette_ui.h — Color Palette Viewer (Test Menu entry)
//
// Screen 1 (grid): every named theme color as a swatch (2 per row,
//   scrollable), plus a color-wheel picker at the bottom to test
//   arbitrary colors against the physical panel.
// Screen 2 (detail): tap a swatch to see it filling ~80% of the
//   screen with its name + hex code, and a back button to the grid.
// ================================================================
#include "lvgl.h"

// Create both screens once (called from test_menu_init()).
void color_palette_ui_create(void);

// Returns the cached grid screen (the Test Menu entry point).
lv_obj_t *color_palette_ui_get_screen(void);
