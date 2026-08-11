#pragma once
// ================================================================
// test_menu.h — Test Features: numbered list of dev/diagnostic UIs
//
// Doc 179 §5/D3 restructure (2026-08-06): no longer a bottom-nav tab.
// Reached only from Settings -> "Test Features" (settings_screen.c
// calls test_menu_show()). Lists every registered test screen by
// number + name; tapping a row builds it ON FIRST TAP (not eagerly at
// boot — doc 179 §2/Phase 1d RAM discipline: this list can include
// screens with their own lv_timers and heavy layouts, and Test
// Features may never be opened in a given session at all) and loads
// it. New test UIs are added by extending the registry in
// test_menu.c — nothing else in the app needs to change.
// ================================================================
#include "lvgl.h"

// Builds the Test Features LIST screen itself (cheap — a handful of
// rows) — NOT the sub-screens it links to, which are created lazily on
// first tap. Call once, at boot (ui_init()).
void test_menu_init(void);

// Loads the Test Features list — called from Settings's "Test
// Features" row.
void test_menu_show(void);

// Returns to the Test Features list — called by every test screen's
// back button.
void test_menu_return(void);
