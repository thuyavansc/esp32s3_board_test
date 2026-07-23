#pragma once
// ================================================================
// test_menu.h — Test Menu: numbered list of test UIs
//
// Reached from the main nav's TEST tab. Lists every registered test
// screen by number + name; tapping a row loads that screen. New test
// UIs are added by extending the registry in test_menu.c — nothing
// else in the app needs to change.
// ================================================================
#include "lvgl.h"

// Creates every registered test screen + the menu list itself.
// Returns the menu screen (stored by ui_main.c as SCREEN_TEST).
lv_obj_t *test_menu_init(void);

// Returns to the Test Menu list — called by every test screen's back button.
void test_menu_return(void);
