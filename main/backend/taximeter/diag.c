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
#include "diag.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

static const char *TAG = "diag";

// ═══════════════════════════════════════════════════════════════
//  "mem" — RAM
// ═══════════════════════════════════════════════════════════════
static void _cmd_mem(void) {
    size_t free_now    = esp_get_free_heap_size();
    size_t free_min_ever = esp_get_minimum_free_heap_size();
    size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t largest_dma  = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "MEMORY (RAM)");
    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  Free heap NOW:          %6u KB", (unsigned)(free_now / 1024));
    ESP_LOGI(TAG, "  Lowest free heap EVER:  %6u KB  \xE2\x86\x90 the real danger number — how close this", (unsigned)(free_min_ever / 1024));
    ESP_LOGI(TAG, "                                     device has come to running out since boot");
    ESP_LOGI(TAG, "  Largest free block:     %6u KB  (8-bit-capable — general allocations)", (unsigned)(largest_8bit / 1024));
    ESP_LOGI(TAG, "  Largest DMA block:      %6u KB  (needed by the display driver's draw buffers)", (unsigned)(largest_dma / 1024));

    #if CONFIG_SPIRAM
        size_t psram_total = esp_psram_get_size();
        size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "  PSRAM total:            %6u KB", (unsigned)(psram_total / 1024));
        ESP_LOGI(TAG, "  PSRAM free:             %6u KB", (unsigned)(psram_free / 1024));
    #else
        ESP_LOGI(TAG, "  PSRAM:                  not present on this build");
    #endif

    ESP_LOGI(TAG, "──────────────────────────────────────");
    if (free_now < 20 * 1024) {
        ESP_LOGW(TAG, "  \xE2\x9A\xA0 Free heap is under 20KB — an HTTPS/TLS call right now may fail");
        ESP_LOGW(TAG, "    (mbedTLS alone can need ~16-20KB contiguous).");
    } else {
        ESP_LOGI(TAG, "  Heap headroom looks healthy for an HTTPS call right now.");
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
    if (strncmp(line, "diag", 4) == 0) {
        _show_help();
        return true;
    }
    return false;
}
