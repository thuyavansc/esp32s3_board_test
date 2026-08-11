/**
 * diag.c — "What's using RAM right now?" / "What's actually stored?"
 *
 * See diag.h for why this exists. Two commands, both read-only,
 * touching nothing else's state — safe to run at any time.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "config.h"
#include "session_store.h"
#include "ui_main.h"   // ui_get_lvgl_mem_stats() — plain cached struct, no LVGL call from this task
#include "diag.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

static const char *TAG = "diag";

// ═══════════════════════════════════════════════════════════════
//  "stacks" — per-task high-water marks (doc 184 §10.2)
//
//  This project had ZERO stack instrumentation until this — which is
//  exactly why nobody could see trip_tick's 4096-byte stack was too
//  small before it overflowed 3 times (doc 184 §1). uxTaskGetStack
//  HighWaterMark() is unconditionally available on ESP-IDF (INCLUDE_
//  uxTaskGetStackHighWaterMark is hardcoded =1 in FreeRTOSConfig.h,
//  not a Kconfig option) — no sdkconfig change needed. Not every task
//  in this firmware is registered, only the ones that do blocking
//  network/UART I/O or otherwise matter for stack sizing — see each
//  xTaskCreate() call site for where diag_register_task() was added.
// ═══════════════════════════════════════════════════════════════
#define DIAG_MAX_TRACKED_TASKS 8
typedef struct {
    TaskHandle_t handle;
    char         name[16];
} _tracked_task_t;
static _tracked_task_t s_tasks[DIAG_MAX_TRACKED_TASKS];
static int s_task_count = 0;

void diag_register_task(TaskHandle_t handle, const char *name) {
    if (!handle || s_task_count >= DIAG_MAX_TRACKED_TASKS) return;
    s_tasks[s_task_count].handle = handle;
    strlcpy(s_tasks[s_task_count].name, name ? name : "?", sizeof(s_tasks[s_task_count].name));
    s_task_count++;
}

static void _cmd_stacks(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "TASK STACK HIGH-WATER MARKS");
    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  \"free\" = the LOWEST this task's stack has ever come down to since");
    ESP_LOGI(TAG, "  boot — not current usage. Low/zero here means it nearly (or did)");
    ESP_LOGI(TAG, "  overflow at some point — see doc 184 §1/§10 for why this matters.");
    if (s_task_count == 0) {
        ESP_LOGI(TAG, "  (no tasks registered — see diag_register_task())");
    }
    for (int i = 0; i < s_task_count; i++) {
        UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(s_tasks[i].handle);
        const char *flag = (free_bytes < 512)  ? "  <-- LOW, investigate" :
                            (free_bytes < 1024) ? "  <-- getting tight"    : "";
        ESP_LOGI(TAG, "  %-14s %6u bytes free (minimum ever)%s", s_tasks[i].name, (unsigned)free_bytes, flag);
    }
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

// ═══════════════════════════════════════════════════════════════
//  "mem" — RAM
// ═══════════════════════════════════════════════════════════════
// Phase 0 (doc 155 §12) — this command was REWRITTEN because the old
// version was actively misleading: it reported esp_get_free_heap_size(),
// which sums ALL capabilities. On this board that total is dominated by
// 8MB of PSRAM, so it printed a reassuring "8221 KB free" while INTERNAL
// SRAM — the pool that actually constrains WiFi/USB-host/lwIP/task stacks
// — was down to 38KB. That's why the internal-SRAM problem stayed
// invisible until a `getinfo` happened to break the pools out separately.
// Internal SRAM is now reported FIRST and on its own, because it is the
// number that decides whether a feature fits on this board.
static void _cmd_mem(void) {
    // ── Internal SRAM — the constrained pool ──
    size_t int_total   = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t int_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "MEMORY (RAM)");
    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  INTERNAL SRAM  \xE2\x86\x90 the pool that actually limits this board");
    ESP_LOGI(TAG, "    Total:                %6u KB", (unsigned)(int_total / 1024));
    ESP_LOGI(TAG, "    Free NOW:             %6u KB", (unsigned)(int_free / 1024));
    ESP_LOGI(TAG, "    Lowest EVER:          %6u KB  \xE2\x86\x90 how close we've come to running out", (unsigned)(int_min / 1024));
    ESP_LOGI(TAG, "    Largest free block:   %6u KB  \xE2\x86\x90 a big single alloc can't exceed this,", (unsigned)(int_largest / 1024));
    ESP_LOGI(TAG, "                                     no matter how much total is free");
    ESP_LOGI(TAG, "    Largest DMA block:    %6u KB  (display draw buffers, WiFi, USB host)", (unsigned)(largest_dma / 1024));

    #if CONFIG_SPIRAM
        size_t psram_total   = esp_psram_get_size();
        size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "  PSRAM  (plentiful — but CANNOT serve DMA or <16KB allocations)");
        ESP_LOGI(TAG, "    Total:                %6u KB", (unsigned)(psram_total / 1024));
        ESP_LOGI(TAG, "    Free NOW:             %6u KB", (unsigned)(psram_free / 1024));
        ESP_LOGI(TAG, "    Largest free block:   %6u KB", (unsigned)(psram_largest / 1024));
    #else
        ESP_LOGI(TAG, "  PSRAM:                  not present on this build");
    #endif

    // ── LVGL's own static pool — the biggest internal-SRAM consumer ──
    // Read from the cached snapshot the dashboard lv_timer maintains;
    // this never calls into LVGL from this task (see ui_main.h).
    ui_lvgl_mem_stats_t lv;
    ui_get_lvgl_mem_stats(&lv);
    ESP_LOGI(TAG, "  LVGL POOL  (static, carved out of internal SRAM above)");
    if (!lv.valid) {
        ESP_LOGI(TAG, "    (no sample yet — the dashboard timer fills this in ~1s after ui_init,");
        ESP_LOGI(TAG, "     or LV_MEM_CUSTOM=1 is set, in which case LVGL has no pool to measure)");
    } else {
        ESP_LOGI(TAG, "    Pool size:            %6u KB  (= CONFIG_LV_MEM_SIZE_KILOBYTES)", (unsigned)(lv.total_bytes / 1024));
        ESP_LOGI(TAG, "    Used now:             %6u KB  (%u%%)", (unsigned)(lv.used_bytes / 1024), (unsigned)lv.used_pct);
        ESP_LOGI(TAG, "    PEAK used since boot: %6u KB  \xE2\x86\x90 size the pool from THIS, + headroom", (unsigned)(lv.max_used_bytes / 1024));
        ESP_LOGI(TAG, "    Largest free in pool: %6u KB  | fragmentation %u%%", (unsigned)(lv.free_biggest / 1024), (unsigned)lv.frag_pct);
        ESP_LOGI(TAG, "    (visit every screen + run a trip before trusting PEAK — it only");
        ESP_LOGI(TAG, "     reflects what the UI has actually been asked to draw so far)");
    }

    ESP_LOGI(TAG, "──────────────────────────────────────");
    if (int_free < 20 * 1024) {
        ESP_LOGW(TAG, "  \xE2\x9A\xA0 Internal SRAM under 20KB — an HTTPS/TLS call may fail right now");
        ESP_LOGW(TAG, "    (mbedTLS is on PSRAM here, but WiFi/lwIP/task stacks still need internal).");
    } else if (int_free < 60 * 1024) {
        ESP_LOGW(TAG, "  \xE2\x9A\xA0 Internal SRAM under 60KB — too tight to add USB-host + SoftAP + PPP");
        ESP_LOGW(TAG, "    (doc 155 §6 estimates that feature needs 55-80KB internal).");
    } else {
        ESP_LOGI(TAG, "  Internal SRAM headroom looks healthy.");
    }
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

// ═══════════════════════════════════════════════════════════════
//  "store" — SPIFFS files + NVS session state
// ═══════════════════════════════════════════════════════════════
static void _list_dir(const char *path, const char *label) {
    DIR *dir = opendir(path);
    if (!dir) {
        printf("  %-28s (not found / empty)\n", label);
        return;
    }

    struct dirent *entry;
    int count = 0;
    size_t total_bytes = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        char full_path[300];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;
        printf("    %-40s %8ld bytes\n", entry->d_name, (long)st.st_size);
        total_bytes += (size_t)st.st_size;
        count++;
    }
    closedir(dir);
    printf("  %-28s %d file(s), %u bytes total\n", label, count, (unsigned)total_bytes);
}

static void _cmd_store(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "STORAGE — what's actually stored");
    ESP_LOGI(TAG, "──────────────────────────────────────");

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "  SPIFFS partition: %u KB total | %u KB used | %u KB free",
                 (unsigned)(total / 1024), (unsigned)(used / 1024), (unsigned)((total - used) / 1024));
    } else {
        ESP_LOGW(TAG, "  SPIFFS info unavailable — is it mounted? (rest_api_storage_init() mounts it at boot)");
    }

    printf("\n");
    _list_dir("/spiffs" STORAGE_DIR, "trip-fetch cache (/store):");
    _list_dir("/spiffs" STORAGE_DIR "/ref", "reference data (/store/ref):");
    _list_dir("/spiffs" STORAGE_DIR "/trips", "local trips (/store/trips):");
    printf("\n");

    ESP_LOGI(TAG, "  (use 'ref list tariffs|fixedrates|specialfares|holidays' to see PARSED reference");
    ESP_LOGI(TAG, "   data row-by-row, and 'trip info' for the currently active trip's live totals)");
    ESP_LOGI(TAG, "──────────────────────────────────────");

    session_store_print();
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

static void _show_help(void) {
    printf("\n  mem       Free heap / largest block / PSRAM\n");
    printf("  store     SPIFFS usage + every stored file + full session (NVS) dump\n");
    printf("  stacks    Per-task stack high-water marks (doc 184 §10.2)\n");
    printf("  diag help Show this help\n\n");
}

bool diag_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;

    if (strcmp(line, "mem") == 0) {
        _cmd_mem();
        return true;
    }
    if (strcmp(line, "store") == 0) {
        _cmd_store();
        return true;
    }
    if (strcmp(line, "stacks") == 0) {
        _cmd_stacks();
        return true;
    }
    if (strncmp(line, "diag", 4) == 0) {
        _show_help();
        return true;
    }
    return false;
}
