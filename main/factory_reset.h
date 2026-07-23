#pragma once
// ================================================================
// factory_reset.h — passcode-gated "revert to factory" serial command.
//
// Always compiled in, regardless of ENABLE_OTA/ENABLE_TRIPS_API/
// ENABLE_MINI_COMMAND — this is a safety net that must survive no matter
// what future bug ships in any feature-flagged module. See
// docs/TestFunctionalities/ota-updates/50_2026-07-10_bad_ota_release_recovery_playbook.md
// for the exact scenario this exists for: a device whose OTA-download
// logic itself is broken can't fix itself over the air (chicken-and-egg),
// but AS LONG AS its serial command reader still works (a separate code
// path from the HTTP/OTA logic), this command reverts it to `factory`
// with nothing more than a typed command — no cable, no rebuild, no
// esptool — because it only flips the boot pointer, it never needs to
// download or write any new code.
//
// FLOW (two lines, on purpose — a single-command trigger would be too
// easy to fire by accident):
//   1. Type: factory reset
//      -> device prints a prompt, arms itself, does NOT reset yet
//   2. Type the passcode (FACTORY_RESET_PASSCODE, config.h — currently
//      "1010") on the very next line
//      -> match: esp_ota_set_boot_partition(factory) + esp_restart()
//      -> mismatch: cancelled, nothing happens
// See docs/TestFunctionalities/ota-updates/48_2026-07-10_esp32_ota_firmware_guide.md
// Section 9 for the full usage walkthrough with example serial output.
// ================================================================
#include <stdbool.h>

// Feed every line from the serial command reader through this FIRST,
// before any other command dispatch — it needs to see every line while
// "armed" (awaiting the passcode), not just lines that look like commands.
// Returns true if this module consumed the line (either as the trigger or
// as a passcode attempt) — false means "not mine, keep dispatching."
bool factory_reset_process_line(const char *line);
