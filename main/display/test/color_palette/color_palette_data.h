#pragma once
// ================================================================
// color_palette_data.h — Named color list for the Color Palette
// Viewer test UI. Every theme entry mirrors a macro in ui_theme.h —
// this is purely a debug/QA view of what the app actually uses, plus
// 5 "Pure" primary/white/black entries (not part of the theme) used
// to sanity-check the raw color pipeline independent of any specific
// theme value.
//
// Some entries share an identical hex value (e.g. Cerulean/Button
// Hover, or Active Green/Success) because they're distinct semantic
// ROLES in the theme that happen to reuse the same color today.
// They're listed separately on purpose, since the point of this
// screen is to verify every role the codebase references renders
// correctly on the physical panel.
//
// NOTE: entries store the raw uint32_t hex code, not an lv_color_t —
// lv_color_hex() is a function call, not a compile-time constant, so
// it can't appear in a static initializer. Convert with
// lv_color_hex(entry->hex_value) at the point of use instead.
// ================================================================
#include <stdint.h>
#include "lvgl.h"

typedef struct {
    const char *name;      // human-readable role name
    const char *macro;     // ui_theme.h macro this comes from
    const char *hex;       // "#RRGGBB" for display
    uint32_t    hex_value; // matches the macro's lv_color_hex() argument
} palette_entry_t;

static const palette_entry_t PALETTE_COLORS[] = {
    // ── Pure diagnostic colors — NOT part of the app theme. If any of
    // these five don't render as their exact name (pure red staying
    // red, pure white staying white, etc.) the display's color/byte
    // order pipeline is still wrong. Listed first so they're the
    // easiest to check. ──
    { "Pure Red",   "-",  "#FF0000", 0xFF0000 },
    { "Pure Green", "-",  "#00FF00", 0x00FF00 },
    { "Pure Blue",  "-",  "#0000FF", 0x0000FF },
    { "Pure White", "-",  "#FFFFFF", 0xFFFFFF },
    { "Pure Black", "-",  "#000000", 0x000000 },

    { "Jet Black",              "C_BG",        "#052C36", 0x052C36 },
    { "Jet Black 2",            "C_BG2",       "#11313A", 0x11313A },
    { "Dark Teal 2",            "C_CARD",      "#073F4A", 0x073F4A },
    { "Dark Teal",              "C_NAV_BG",    "#025773", 0x025773 },
    { "Baltic Blue",            "C_BTN",       "#095B78", 0x095B78 },
    { "Cerulean",               "C_BTN_HOVER", "#166B87", 0x166B87 },
    { "Cerulean (Accent)",      "C_ACCENT",    "#166B87", 0x166B87 },
    { "Cerulean 2",             "C_ACCENT2",   "#297591", 0x297591 },
    { "Baltic Blue (Mid)",      "C_MID",       "#095B78", 0x095B78 },
    { "Active Green",           "C_NAV_ACT",   "#00C97A", 0x00C97A },
    { "Active Green (Success)", "C_SUCCESS",   "#00C97A", 0x00C97A },
    { "Warm Orange",            "C_WARN",      "#FF9F40", 0xFF9F40 },
    { "Alert Red",              "C_ERROR",     "#E05050", 0xE05050 },
    { "Bright White",           "C_TEXT",      "#F0F4F5", 0xF0F4F5 },
    { "Light Cyan",             "C_TEXT2",     "#A8D8E8", 0xA8D8E8 },
    { "Divider",                "C_DIVIDER",   "#0D4555", 0x0D4555 },
    { "Test BG",                "C_TEST_BG",   "#1A1A1A", 0x1A1A1A },
    { "Test Card",              "C_TEST_CARD", "#242424", 0x242424 },
    { "Test Header",            "C_TEST_HDR",  "#2A2A2A", 0x2A2A2A },
};

#define PALETTE_COLOR_COUNT (sizeof(PALETTE_COLORS) / sizeof(PALETTE_COLORS[0]))
