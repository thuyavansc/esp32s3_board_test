#pragma once
// ================================================================
// ram_test.h — SRAM + PSRAM confirmation, new for esp32s3_board.
//
// Why this exists: chip_info_test's own heap report only ever printed
// esp_get_free_heap_size()/MALLOC_CAP_8BIT totals — that number is
// dominated by internal SRAM and never proves PSRAM is actually usable
// (a board could have completely dead PSRAM and this number wouldn't
// change at all, since heap_caps only reports what CONFIG_SPIRAM=y +
// physical presence made available in the first place). This module
// does a REAL exercise: allocate → fill with a non-trivial pattern →
// read back and verify byte-for-byte → free, separately for internal
// SRAM (MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT) and PSRAM
// (MALLOC_CAP_SPIRAM) — so "it works" means "data survived a real
// write/read cycle," not just "malloc() returned non-NULL."
//
// SRAM already works as expected (per prior testing on this board
// family) — it's covered on-demand only ("ram test sram"). PSRAM is
// the one actually being confirmed here, so it additionally gets its
// own periodic background task (see ram_test_init()) that keeps
// re-proving it works for as long as the device stays powered on, not
// just once at boot.
// ================================================================
#include <stdbool.h>

// Call once from app_main() (after nvs_flash_init()/WiFi init order
// doesn't matter — this module has no network dependency). Logs
// esp_psram_is_initialized()/esp_psram_get_size() once, then starts the
// periodic PSRAM health-check task (config.h's
// RAM_TEST_PSRAM_TASK_FIRST_RUN_DELAY_S / _INTERVAL_S).
void ram_test_init(void);

// "ram test sram" | "ram test psram" | "ram test download" | "ram test all"
// | "ram info" from the serial command reader ("download" is implemented
// in psram_download_test.c — a real network download buffered in PSRAM,
// as opposed to this module's synthetic pattern-fill test). "ram info" is
// read-only (no allocation) — live totals/free/largest-block for both
// capabilities. Returns true if recognized.
bool ram_test_process_command(const char *line);

// Logs live SRAM+PSRAM totals/free/largest-block with a caller-supplied
// label — the same snapshot style this module's own tests use, exposed
// so other modules (e.g. psram_download_test.c) can report "before/
// during/after" snapshots around their own real allocations without
// duplicating the heap_caps_get_*() reporting logic.
void ram_test_log_snapshot(const char *label);
