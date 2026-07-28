#pragma once
// ================================================================
// sms_screen.h — SMS inbox + send GUI (Phase 2, doc 155/159)
//
// Reached via Test Menu -> "SMS (Inbox/Send)" — same drill-down
// convention as network_screen.c/test_pax_meter.c/color_palette_ui.c.
//
// Shows the in-RAM inbox (sms_client_get_inbox()) and a "+ Send New
// SMS" button that collects a recipient number then a message body via
// two chained text_keypad_show() popups (same two-step pattern
// network_screen.c already uses for hotspot password change), then
// sends via gps_client_send_sms() routed through bg_worker (blocks up
// to ~20s — never called directly from the LVGL thread).
// ================================================================
#include "lvgl.h"

// Creates the screen. Called once from test_menu_init().
void sms_screen_create(void);

// Returns the cached screen — also opportunistically refreshes the
// inbox list (cheap, no AT traffic — just a RAM copy) before returning,
// same convention as network_screen_get_screen().
lv_obj_t *sms_screen_get_screen(void);
