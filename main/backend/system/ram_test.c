#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "config.h"
#include "ram_test.h"
#include "psram_download_test.h"

static const char *TAG = "ram_test";

// Formats a byte count as "X.X KB" (under 1MB) or "X.XX MB" (1MB+), so every
// raw byte number this module logs also has a human-readable size next to
// it — plain byte counts are hard to eyeball/compare at a glance. A small
// rotating pool of buffers (not just one) lets several calls appear safely
// within the same single ESP_LOGI/printf line (the widest line here uses 4).
static const char *_fmt(size_t bytes) {
    static char buf[6][24];
    static int  idx = 0;
    idx = (idx + 1) % 6;
    if (bytes < 1024u * 1024u) {
        snprintf(buf[idx], sizeof(buf[idx]), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf[idx], sizeof(buf[idx]), "%.2f MB", bytes / (1024.0 * 1024.0));
    }
    return buf[idx];
}

typedef struct {
    const char *label;      // "SRAM" or "PSRAM"
    uint32_t    caps;       // MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT, or MALLOC_CAP_SPIRAM
    size_t      test_size;
    bool        last_pass;
    size_t      last_used_bytes;
} ram_region_t;

static ram_region_t s_sram  = { "SRAM",  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, RAM_TEST_SRAM_SIZE_BYTES,  false, 0 };
static ram_region_t s_psram = { "PSRAM", MALLOC_CAP_SPIRAM,                     RAM_TEST_PSRAM_SIZE_BYTES, false, 0 };

// ── The actual test: allocate -> write a non-trivial pattern -> read
// back and verify every byte -> free. Reports live before/after numbers
// from heap_caps_get_*() — nothing here is a hardcoded/assumed value.
static bool _run_region_test(ram_region_t *r) {
    size_t free_before   = heap_caps_get_free_size(r->caps);
    size_t largest_before = heap_caps_get_largest_free_block(r->caps);

    ESP_LOGI(TAG, "── %s test: requesting %u bytes (%s) ──", r->label, (unsigned)r->test_size, _fmt(r->test_size));
    ESP_LOGI(TAG, "  Before: free=%u bytes (%s) | largest free block=%u bytes (%s)",
             (unsigned)free_before, _fmt(free_before), (unsigned)largest_before, _fmt(largest_before));

    uint8_t *buf = (uint8_t *)heap_caps_malloc(r->test_size, r->caps);
    if (!buf) {
        ESP_LOGE(TAG, "  FAIL — heap_caps_malloc(%u bytes / %s, caps=0x%x) returned NULL",
                 (unsigned)r->test_size, _fmt(r->test_size), (unsigned)r->caps);
        r->last_pass = false;
        r->last_used_bytes = 0;
        return false;
    }

    size_t free_after_alloc = heap_caps_get_free_size(r->caps);

    // Address-derived counting pattern — NOT all-0x00 / all-0xFF, so a
    // stuck-bit or address-line fault is actually caught by the verify
    // step below (an all-0x00 pattern can't distinguish "wrote correctly"
    // from "chip stuck at 0").
    for (size_t i = 0; i < r->test_size; i++) {
        buf[i] = (uint8_t)((i * 2654435761u) >> 3);
    }

    bool ok = true;
    size_t mismatch_offset = 0;
    for (size_t i = 0; i < r->test_size; i++) {
        uint8_t expected = (uint8_t)((i * 2654435761u) >> 3);
        if (buf[i] != expected) {
            ok = false;
            mismatch_offset = i;
            break;
        }
    }

    heap_caps_free(buf);
    size_t free_after_free = heap_caps_get_free_size(r->caps);
    size_t largest_after_free = heap_caps_get_largest_free_block(r->caps);

    size_t used_by_test = (free_before > free_after_alloc) ? (free_before - free_after_alloc) : 0;

    if (ok) {
        ESP_LOGI(TAG, "  Integrity: PASS — %u bytes (%s) written and read back correctly",
                 (unsigned)r->test_size, _fmt(r->test_size));
    } else {
        ESP_LOGE(TAG, "  Integrity: FAIL — first mismatch at offset %u",
                 (unsigned)mismatch_offset);
    }
    ESP_LOGI(TAG, "  During: used_by_test=%u bytes (%s) | free_after_alloc=%u bytes (%s)",
             (unsigned)used_by_test, _fmt(used_by_test), (unsigned)free_after_alloc, _fmt(free_after_alloc));
    ESP_LOGI(TAG, "  After free: free=%u bytes (%s) | largest free block=%u bytes (%s)",
             (unsigned)free_after_free, _fmt(free_after_free), (unsigned)largest_after_free, _fmt(largest_after_free));

    r->last_pass = ok;
    r->last_used_bytes = used_by_test;
    return ok;
}

// Shared snapshot core — used by both "ram info" (no label, standalone
// report) and ram_test_log_snapshot() (labeled, used by other modules
// like psram_download_test.c around their own real allocations).
static void _log_both_pools(const char *label) {
    if (label) {
        ESP_LOGI(TAG, "-- RAM snapshot: %s --", label);
    } else {
        ESP_LOGI(TAG, "======================================");
        ESP_LOGI(TAG, "RAM INFO (live — no allocation)");
        ESP_LOGI(TAG, "--------------------------------------");

        bool psram_ok = esp_psram_is_initialized();
        ESP_LOGI(TAG, "  PSRAM initialized: %s", psram_ok ? "YES" : "NO");
        if (psram_ok) {
            ESP_LOGI(TAG, "  PSRAM total (esp_psram_get_size): %u bytes (%.2f MB)",
                     (unsigned)esp_psram_get_size(), esp_psram_get_size() / (1024.0 * 1024.0));
        }
    }

    {
        size_t sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t sram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t sram_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t sram_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "  SRAM  (MALLOC_CAP_INTERNAL) total=%u (%s) free=%u (%s) min-free=%u (%s) largest-block=%u (%s)",
                 (unsigned)sram_total, _fmt(sram_total), (unsigned)sram_free, _fmt(sram_free),
                 (unsigned)sram_min, _fmt(sram_min), (unsigned)sram_block, _fmt(sram_block));

        size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "  PSRAM (MALLOC_CAP_SPIRAM)   total=%u (%s) free=%u (%s) min-free=%u (%s) largest-block=%u (%s)",
                 (unsigned)psram_total, _fmt(psram_total), (unsigned)psram_free, _fmt(psram_free),
                 (unsigned)psram_min, _fmt(psram_min), (unsigned)psram_block, _fmt(psram_block));
    }
}

static void _print_ram_info(void) {
    _log_both_pools(NULL);
}

void ram_test_log_snapshot(const char *label) {
    _log_both_pools(label);
}

// ── Periodic PSRAM health-check task ────────────────────────────
// Runs once shortly after boot (proves PSRAM at startup, after
// WiFi/NVS/SPIFFS have already claimed their share of internal SRAM),
// then repeats every RAM_TEST_PSRAM_TASK_INTERVAL_S seconds so a fault
// appearing later during long-running operation is also caught,
// unattended, in the serial log.
static void _psram_health_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(RAM_TEST_PSRAM_TASK_FIRST_RUN_DELAY_S * 1000));

    while (1) {
        ESP_LOGI(TAG, "══════════════════════════════════════");
        ESP_LOGI(TAG, "PSRAM periodic health-check");
        _run_region_test(&s_psram);
        ESP_LOGI(TAG, "══════════════════════════════════════");
        vTaskDelay(pdMS_TO_TICKS(RAM_TEST_PSRAM_TASK_INTERVAL_S * 1000));
    }
}

void ram_test_init(void) {
    bool psram_ok = esp_psram_is_initialized();
    if (psram_ok) {
        ESP_LOGI(TAG, "PSRAM detected — %u bytes (%.2f MB) total",
                 (unsigned)esp_psram_get_size(), esp_psram_get_size() / (1024.0 * 1024.0));
    } else {
        ESP_LOGW(TAG, "PSRAM NOT detected — check CONFIG_SPIRAM=y (sdkconfig.defaults) and the "
                      "physical board; 'ram test psram' will fail until this is fixed");
    }

    xTaskCreate(_psram_health_task, "psram_health", 4096, NULL, 1, NULL);
}

static void _show_help(void) {
    printf("\n  ram info          Live SRAM+PSRAM totals/free/largest-block (no allocation)\n");
    printf("  ram test sram     Allocate/write/verify/free %u bytes (%s) of internal SRAM\n", (unsigned)RAM_TEST_SRAM_SIZE_BYTES, _fmt(RAM_TEST_SRAM_SIZE_BYTES));
    printf("  ram test psram    Allocate/write/verify/free %u bytes (%s) of PSRAM (synthetic pattern)\n", (unsigned)RAM_TEST_PSRAM_SIZE_BYTES, _fmt(RAM_TEST_PSRAM_SIZE_BYTES));
    printf("  ram test download Real HTTPS download, buffered ENTIRELY in PSRAM, verified by size+SHA-256\n");
    printf("  ram test all      Run SRAM + PSRAM (synthetic) tests\n\n");
}

bool ram_test_process_command(const char *line) {
    while (*line == ' ') line++;
    if (strncasecmp(line, "ram", 3) != 0) return false;

    const char *p = line + 3;
    while (*p == ' ') p++;

    if (*p == '\0' || strncasecmp(p, "help", 4) == 0) { _show_help(); return true; }
    if (strncasecmp(p, "info", 4) == 0) { _print_ram_info(); return true; }

    if (strncasecmp(p, "test", 4) == 0) {
        p += 4;
        while (*p == ' ') p++;

        if (strncasecmp(p, "sram", 4) == 0) {
            _run_region_test(&s_sram);
            return true;
        }
        if (strncasecmp(p, "psram", 5) == 0) {
            _run_region_test(&s_psram);
            return true;
        }
        if (strncasecmp(p, "download", 8) == 0) {
            psram_download_test_run();
            return true;
        }
        if (strncasecmp(p, "all", 3) == 0 || *p == '\0') {
            bool sram_ok  = _run_region_test(&s_sram);
            bool psram_ok = _run_region_test(&s_psram);
            ESP_LOGI(TAG, "SUMMARY: SRAM=%s | PSRAM=%s",
                     sram_ok ? "PASS" : "FAIL", psram_ok ? "PASS" : "FAIL");
            return true;
        }

        printf("Unknown 'ram test' target — try 'ram help'\n");
        return true;
    }

    printf("Unknown ram command — try 'ram help'\n");
    return true;
}
