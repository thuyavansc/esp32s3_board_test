#pragma once
// ================================================================
// psram_download_test.h — real HTTPS download, buffered ENTIRELY in
// PSRAM (deliberately NOT streamed to flash/SPIFFS the way
// trips_api.c/ota_client.c do), then verified by size + SHA-256.
//
// Why this exists, separate from ram_test.c's synthetic PSRAM test:
// ram_test.c proves PSRAM can hold and correctly return a pattern IT
// wrote itself. This module proves the more realistic case — a real,
// size-proportional PSRAM allocation driven by genuine network I/O
// (the same shape as "download a whole image/video into RAM" would
// be), with the actual downloaded bytes verified against a known-good
// SHA-256 instead of a synthetic fill pattern.
//
// Target file/hash (config.h's PSRAM_DL_TEST_URL/PSRAM_DL_TEST_SHA256)
// come from a real manifest response already returned by this
// project's own existing OTA test infrastructure — not invented.
//
// Deliberately does NOT stream: the whole point is to exercise "hold
// the complete response in one PSRAM buffer at once" (doc 108/109's
// "buffer-the-whole-thing" case), as the direct real-world contrast to
// trips_api.c/ota_client.c's streaming approach.
// ================================================================
#include <stdbool.h>

// Runs the download -> allocate -> verify (size + SHA-256) -> free
// cycle once, synchronously. Logs SRAM+PSRAM snapshots before, during,
// and after (via ram_test_log_snapshot()), plus a clear PASS/FAIL for
// both the size check and the integrity (SHA-256) check. Safe to call
// from the serial command task. Requires WiFi already connected
// (ENABLE_WIFI in config.h).
void psram_download_test_run(void);
