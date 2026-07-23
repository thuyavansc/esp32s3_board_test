#pragma once
// ================================================================
// test_pax_meter.h — PAX A920Pro taxi meter clone (Test Menu entry)
//
// Reference: docs/TestFunctionalities/display/ui design-display sample2.jpeg
// Reached via Test Menu → "PAX A920Pro Meter UI". Unchanged visuals
// from the original ui_main.c _create_test() — only the navigation
// (back header instead of the bottom 5-tab nav bar) changed.
// ================================================================
#include "lvgl.h"

// Create the screen once (called from test_menu_init()).
void test_pax_meter_create(void);

// Returns the cached screen (NULL until test_pax_meter_create() has run).
lv_obj_t *test_pax_meter_get_screen(void);
