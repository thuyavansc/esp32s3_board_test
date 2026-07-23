#pragma once
// ================================================================
// trips_api.h — HTTPS Trips fetch + SPIFFS JSON storage (v3+ feature)
//
// Ported from esp32_wifi_rest_api/main/rest_api_storage.c, trimmed down:
//   - SPIFFS-only storage (that project's SD/PSRAM/SRAM backend options
//     removed — this test firmware never needs them)
//   - No GPS, no iStartek/G2 sockets — this project only tests the HTTPS
//     API-call + OTA path, per the instruction to leave those out
//   - WiFi SSID/password unchanged from every other project this session
//
// Serial commands (only registered when ENABLE_TRIPS_API=1, config.h):
//   api get [id]      Fetch trip JSON from the API, store to SPIFFS
//   api read [id]     Read a stored trip JSON, print to Serial Monitor
//   api list          List every stored trip file with its size
//   api delete <id|all>  Delete one stored file, or all of them
//   api info          Storage backend + free space + fetch stats
//   api help          Show this command list
//
// Entirely gated behind #if ENABLE_TRIPS_API — when that flag is 0 in
// config.h, this whole module compiles to a no-op (init just logs
// "disabled" and returns), matching the same enable/disable pattern
// already used everywhere else in this codebase this session.
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

// Mounts SPIFFS "storage" partition. Call once, after WiFi connects.
esp_err_t trips_api_init(void);

// Fetch a trip's JSON over HTTPS and store it. Public — callable directly
// from code (not just the serial command), same as every other module
// this session exposes both a serial command AND a plain function.
esp_err_t trips_api_fetch(int trip_id);

// Parses "api <cmd> [arg]" from the serial command reader. Returns true
// if recognized (and handled) — false means "not an api command."
bool trips_api_process_command(const char *line);
