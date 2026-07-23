#pragma once
#include <stddef.h> // size_t — version.h must be self-contained since config.h
                     // includes it before anything else
// ================================================================
// version.h — Firmware version for esp32s3_board, defined ONCE here.
//
// Ported from esp32_chip_info_test/main/version.h — same two display
// formats, same VERSION_DISPLAY_LEGACY_SIXFIELD switch in config.h, same
// FW_VERSION_CODE-is-what-OTA-compares-on rule (kept even though
// ENABLE_OTA=0 right now — flipping it back on later needs no version.h
// changes). See that file's own history for the full rationale; only the
// project identity fields below were changed for this new project.
//
// HOW TO CUT A NEW VERSION:
//   1. Set FW_VERSION_YEAR/MONTH/DAY to today's date.
//   2. Bump FW_VERSION_CODE by exactly +1.
//   3. (Legacy scheme only) Also bump FW_VERSION_MAJOR/MINOR/BUILD.
// ================================================================

// Legacy fields — kept, only consumed when VERSION_DISPLAY_LEGACY_SIXFIELD=1
// (config.h). Harmless to leave defined either way.
#define FW_VERSION_MAJOR   1
#define FW_VERSION_MINOR   0
#define FW_VERSION_BUILD   1

// Used by BOTH display schemes — set these to today's date every release.
#define FW_VERSION_YEAR    26
#define FW_VERSION_MONTH   7
#define FW_VERSION_DAY     23

// The ONLY field OTA "is this newer" comparisons use — bump by exactly +1
// every release, never skip/reuse/go backwards.
#define FW_VERSION_CODE    1

// Project name embedded in every log line and (if OTA is ever re-enabled)
// the base of the auto-renamed .bin filename.
#define FW_PROJECT_NAME    "esp32s3_board"

// Human-readable label for whichever feature set is enabled right now —
// purely cosmetic (boot banner / getversion output). Update alongside the
// ENABLE_* flags in config.h when you change them.
#define FW_BUILD_LABEL     "v1: ESP32-S3 N16R8V board bring-up — chip/flash/SRAM/PSRAM diagnostics, OTA + remote-config present but disabled"

// ── Public API ─────────────────────────────────────────────────
void version_get_string(char *out, size_t out_len);
int version_get_major_minor(void);
int version_get_code(void);
void version_print_banner(void);
