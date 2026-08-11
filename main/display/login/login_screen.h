#pragma once
// ================================================================
// login_screen.h — Login screen (doc 179 §5/Phase 2, 2026-08-06)
//
// Shown FIRST, before the meter/trips/settings tabs. Username +
// password fields (tap either -> text_keypad, the same keyboard used
// by the SMS send screen, per your ask), a password eye-toggle
// (text_keypad's own new feature, doc 179 D6), a "Remember me"
// checkbox, and a LOGIN button. Pre-filled with config.h's
// AUTH_TEST_USERNAME/PASSWORD when LOGIN_PREFILL_DEV_CREDENTIALS=1, so
// one tap logs in during development.
//
// GATE MODE (D1c): session_store_get_login_gate_hard() decides whether
// a "Skip (dev)" link is shown next to LOGIN. A production build
// (config.h BUILD_IS_PRODUCTION=1) NEVER shows it — that check is
// absolute (session_store.c). A dev build defaults to showing it, but
// this can be switched on/off at RUNTIME (no rebuild) via the "login
// gate hard|soft|info" serial command this module registers, or from
// the Settings screen.
//
// REMEMBER ME (D2c): stores the USERNAME only (never the password) via
// session_store_set_remember_me(). On boot, ui_main.c/app_main.c skips
// straight to this screen's login success path ONLY IF remember-me is
// set AND the stored access token is still valid — an expired token
// still requires typing the password.
// ================================================================
#include <stdbool.h>
#include "lvgl.h"

void login_screen_create(void);
lv_obj_t *login_screen_get_screen(void);

// True if a remembered session is currently valid enough to skip this
// screen entirely at boot (doc 179 D2c) — call from app_main.c/ui_main.c
// right after ui_init() to decide the very first screen shown.
bool login_screen_can_auto_login(void);

// Serial command handler ("login gate hard|soft|info").
bool login_screen_process_command(const char *line);
