#pragma once
// ================================================================
// ota_client.h — OTA check-in + download (v2+ feature, ENABLE_OTA)
//
// Talks to the ota_update_server backend (test-codes/ota_update_server)
// using the same endpoint shapes described in
// docs/TestFunctionalities/ota-updates/ESP32S3 Taxi App OTA Update RnD (2).md
// Section 6 (POST /api/ota/check, GET /api/ota/firmware/{releaseId},
// POST /api/ota/status) — a pull-based check, per
// docs/TestFunctionalities/ota-updates/47_..._ota_trigger_strategies_and_protocols.md's
// recommendation (Section 7): polling, not a persistent push connection,
// since this is by far the cheapest option in memory terms and firmware
// availability isn't a real-time event.
//
// Trigger points, all funnelling into the same ota_client_check_now():
//   1. Once, a few seconds after boot (app_main.c)
//   2. Every OTA_CHECK_INTERVAL_S seconds thereafter (background task)
//   3. On demand via the "ota check" serial command
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

// Starts the background periodic-check task. Call once, after WiFi is
// connected. Does an immediate check first, then repeats on the
// OTA_CHECK_INTERVAL_S timer (config.h).
void ota_client_init(void);

// Runs one full check-in -> (maybe) download -> apply cycle right now,
// synchronously. Safe to call from the serial command task or the
// background timer task — never call from the LVGL/UI thread on projects
// that have one (this project doesn't).
void ota_client_check_now(void);

// Parses "ota <cmd>" from the serial command reader ("ota check",
// "ota status"). Returns true if recognized.
bool ota_client_process_command(const char *line);
