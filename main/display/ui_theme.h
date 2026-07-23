#pragma once
// ================================================================
// ui_theme.h — Shared color palette (Cerulean/Teal theme)
//
// Single source of truth for every screen (main 4 screens + all
// test/* screens). Base 7 colors from color palettes.txt, extended
// with contrast/status colors, plus the near-black colors used only
// by the PAX A920Pro test screen.
// ================================================================
#include "lvgl.h"

// ── Background layers (from palette) ──────────────────────────
#define C_BG          lv_color_hex(0x052C36)  // Jet Black (#052C36) — main screen bg
#define C_BG2         lv_color_hex(0x11313A)  // Jet Black 2 (#11313A) — secondary bg areas
#define C_CARD        lv_color_hex(0x073F4A)  // Dark Teal 2 (#073F4A) — card/panel bg

// ── Navigation & interactive (from palette) ────────────────────
#define C_NAV_BG      lv_color_hex(0x025773)  // Dark Teal (#025773) — nav bar bg
#define C_BTN         lv_color_hex(0x095B78)  // Baltic Blue (#095B78) — default buttons
#define C_BTN_HOVER   lv_color_hex(0x166B87)  // Cerulean (#166B87) — pressed/hover state

// ── Accent / Brand (from palette) ─────────────────────────────
#define C_ACCENT      lv_color_hex(0x166B87)  // Cerulean (#166B87) — titles, highlights
#define C_ACCENT2     lv_color_hex(0x297591)  // Cerulean 2 (#297591) — secondary accents
#define C_MID         lv_color_hex(0x095B78)  // Baltic Blue — mid-level elements

// ── Extended: Status colors (added for contrast/readability) ──
#define C_NAV_ACT     lv_color_hex(0x00C97A)  // Active Green — selected nav tab
#define C_SUCCESS     lv_color_hex(0x00C97A)  // Active Green — OK states, START TRIP
#define C_WARN        lv_color_hex(0xFF9F40)  // Warm Orange — warnings, tariff
#define C_ERROR       lv_color_hex(0xE05050)  // Alert Red — errors, END TRIP button

// ── Extended: Text colors (added for contrast/readability) ────
#define C_TEXT        lv_color_hex(0xF0F4F5)  // Bright White — primary text (WCAG AA on #052C36)
#define C_TEXT2       lv_color_hex(0xA8D8E8)  // Light Cyan — labels, secondary info
#define C_DIVIDER     lv_color_hex(0x0D4555)  // Divider lines between sections

// ── Test screen specific (dark near-black, matches PAX device) ─
#define C_TEST_BG     lv_color_hex(0x1A1A1A)  // Near-black — test screen bg
#define C_TEST_CARD   lv_color_hex(0x242424)  // Dark grey — section dividers
#define C_TEST_HDR    lv_color_hex(0x2A2A2A)  // Header bar bg
