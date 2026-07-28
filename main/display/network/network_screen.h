#pragma once
// ================================================================
// network_screen.h — Network status/control GUI (Phase 1, doc 155/158)
//
// Reached via Test Menu → "Network (Cellular/Hotspot)" — same
// drill-down convention as test_pax_meter.c/color_palette_ui.c (own
// _create()/_get_screen() pair, ui_back_header() instead of the main
// 4-tab nav bar, test_menu_return() to go back).
//
// Shows/controls everything Phase 1's backend modules expose:
//   - Active uplink (WiFi/Cellular/Auto) — net_manager.c
//   - Cellular status (USB/PPP state, operator, signal, IP, APN) — cellular_ppp.c
//   - Hotspot (on/off, SSID, client count, NAPT) + SSID/password change,
//     and a "Clients" sub-screen (MAC/IP/RSSI + Kick) — hotspot_ap.c/hotspot_nvs.c
//
// Both screens (main status + client list) live in network_screen.c —
// same "one file, grid+detail" convention color_palette_ui.c already
// uses, since this is one cohesive feature, not two independent ones.
// ================================================================
#include "lvgl.h"

// Creates both screens (main status + client list). Called once from
// test_menu_init(), same convention as every other registered test UI.
void network_screen_create(void);

// Returns the cached main screen (NULL until network_screen_create() has
// run) — also opportunistically refreshes the cheap/direct status
// fields (uplink + hotspot) before returning, since test_menu.c's
// _row_click() calls this right before lv_scr_load(). Cellular status
// (which needs a blocking UART1 AT round-trip) is refreshed separately
// on a background job by this screen's own periodic timer.
lv_obj_t *network_screen_get_screen(void);
