/**
 * rest_api_storage.c — HTTPS REST API Trips Client + JSON Storage
 *
 * See rest_api_storage.h for the full flow/backend description. This
 * is the richer of the two "download a completed server trip" modules
 * that existed across the two source projects being merged here
 * (esp32s3_board_test's own smaller trips_api.c, which hit the exact
 * same URL and did the exact same job, was removed — redundant).
 *
 * Ported from esp32_display_taxi_meter/main/backend/rest_api_storage.c
 * with one structural change: this module no longer runs its own
 * _serial_task — app_main.c's single unified serial_cmd_task now calls
 * rest_api_storage_process_command() directly for any "api ..." line,
 * so two tasks never race to read the same UART.
 *
 * ================================================================
 * STORAGE BACKENDS — HOW DATA IS STORED:
 *
 *   LITTLEFS / SPIFFS / SD Card (STORAGE_BACKEND 1/2/3):
 *     File path: <STORAGE_DIR>/trips_<id>.json
 *     Format:    Raw JSON bytes, exactly as received from HTTP response
 *     Permanent: Yes — survives power-off
 *     Multiple:  Yes — unlimited files (subject to available space)
 *     Delete:    remove() system call deletes the file
 *
 *   PSRAM (STORAGE_BACKEND 4):
 *     Storage:   malloc() in PSRAM via heap_caps_malloc(..., MALLOC_CAP_SPIRAM)
 *     Memory:    Pointer stored in static variable s_psram_current
 *     Format:    Raw JSON bytes, null-terminated
 *     Permanent: No — lost on power-off
 *     Multiple:  No — only ONE JSON at a time (overwrites on next fetch)
 *     Delete:    free(s_psram_current); s_psram_current = NULL;
 *     Size info: s_psram_size tracks current stored size
 *
 *   SRAM (STORAGE_BACKEND 5):
 *     Storage:   Standard malloc() in internal DRAM
 *     Memory:    Pointer stored in static variable s_sram_current
 *     Format:    Raw JSON bytes, null-terminated
 *     Permanent: No — lost on power-off
 *     Multiple:  No — only ONE JSON at a time
 *     Delete:    free(s_sram_current); s_sram_current = NULL;
 *     Size info: s_sram_size tracks current stored size
 *     WARNING:   Limited to ~200 KB free heap. Large JSON will crash.
 *
 * This project defaults to STORAGE_BACKEND=2 (SPIFFS) — see config.h.
 *
 * ================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "config.h"
#include "rest_api_storage.h"
#include "bg_worker.h"

static const char *TAG = "trips_api";

// Maximum bytes to print to serial when reading (prevents flooding)
#define MAX_PRINT_BYTES  4096

// ═══════════════════════════════════════════════════════════════
//  MODULE STATE
// ═══════════════════════════════════════════════════════════════

static bool  s_backend_ready   = false;
static char  s_write_target[16] = "None";
static char  s_storage_mount[32] = "None";

// Current fetch state
static char  s_file_path[256];
static FILE *s_file_handle   = NULL;
static int   s_bytes_written = 0;

// PSRAM backend (STORAGE_BACKEND=4)
#if STORAGE_BACKEND == 4
static char *s_psram_current = NULL;
static int   s_psram_size    = 0;
#endif

// SRAM backend (STORAGE_BACKEND=5)
#if STORAGE_BACKEND == 5
static char *s_sram_current  = NULL;
static int   s_sram_size     = 0;
#endif

// Stats
static int s_total_fetches  = 0;
static int s_total_ok       = 0;
static int s_total_fails    = 0;

// ═══════════════════════════════════════════════════════════════
//  STORAGE BACKEND INIT
// ═══════════════════════════════════════════════════════════════

#if STORAGE_BACKEND == 1
// ── LittleFS Initialization ───────────────────────────────────
#include "esp_littlefs.h"

static bool _init_littlefs(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount FAILED at /littlefs (err=%s)", esp_err_to_name(ret));
        return false;
    }

    char storage_path[64];
    snprintf(storage_path, sizeof(storage_path), "/littlefs%s", STORAGE_DIR);
    struct stat st;
    if (stat(storage_path, &st) != 0) {
        mkdir(storage_path, 0755);
        ESP_LOGI(TAG, "Created storage directory: %s", storage_path);
    }

    strlcpy(s_write_target, "LittleFS", sizeof(s_write_target));
    strlcpy(s_storage_mount, "/littlefs", sizeof(s_storage_mount));

    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS partition: %u KB total | %u KB used | %u KB free",
                 (unsigned)(total/1024), (unsigned)(used/1024), (unsigned)((total-used)/1024));
    }

    ESP_LOGI(TAG, "Storage Backend: LittleFS (Flash, Permanent) ✓");
    return true;
}
#endif

#if STORAGE_BACKEND == 2
// ── SPIFFS Initialization (this project's default) ─────────────
#include "esp_spiffs.h"

static bool _init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount FAILED at /spiffs (err=%s)", esp_err_to_name(ret));
        return false;
    }

    char spiffs_storage_path[64];
    snprintf(spiffs_storage_path, sizeof(spiffs_storage_path), "/spiffs%s", STORAGE_DIR);
    struct stat st;
    if (stat(spiffs_storage_path, &st) != 0) {
        mkdir(spiffs_storage_path, 0755);
        ESP_LOGI(TAG, "Created storage directory: %s", spiffs_storage_path);
    }

    strlcpy(s_write_target, "SPIFFS", sizeof(s_write_target));
    strlcpy(s_storage_mount, "/spiffs", sizeof(s_storage_mount));

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS partition: %u KB total | %u KB used | %u KB free",
                 (unsigned)(total/1024), (unsigned)(used/1024), (unsigned)((total-used)/1024));
    }

    ESP_LOGI(TAG, "Storage Backend: SPIFFS (Flash, Permanent) ✓");
    return true;
}
#endif

#if STORAGE_BACKEND == 3
// ── SD Card (SPI) Initialization ──────────────────────────────
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

static sdmmc_card_t *s_sd_card = NULL;

static bool _init_sd_card(void) {
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_SPI_MOSI,
        .miso_io_num = SD_SPI_MISO,
        .sclk_io_num = SD_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092,
    };
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD SPI bus init FAILED (err=%s)", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_SPI_CS;
    slot_cfg.host_id = SD_SPI_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SD_SPI_FREQ_KHZ;

    sdmmc_card_t *card = NULL;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD Card mount FAILED (err=%s)", esp_err_to_name(ret));
        spi_bus_free(SD_SPI_HOST);
        return false;
    }

    s_sd_card = card;

    if (card) {
        ESP_LOGI(TAG, "SD Card: %s | Size: %llu MB",
                 card->cid.name,
                 (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024*1024)));
    }

    struct stat st;
    char sd_dir[64];
    snprintf(sd_dir, sizeof(sd_dir), "/sdcard%s", STORAGE_DIR);
    if (stat(sd_dir, &st) != 0) {
        mkdir(sd_dir, 0755);
        ESP_LOGI(TAG, "Created SD storage dir: %s", sd_dir);
    }

    strlcpy(s_write_target, "SD Card", sizeof(s_write_target));
    strlcpy(s_storage_mount, "/sdcard", sizeof(s_storage_mount));

    ESP_LOGI(TAG, "Storage Backend: SD Card (SPI, Permanent) ✓");
    return true;
}
#endif

#if STORAGE_BACKEND == 4
// ── PSRAM Initialization ──────────────────────────────────────
#include "esp_psram.h"

static bool _init_psram(void) {
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM NOT available on this board!");
        return false;
    }

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM detected: %u KB total", (unsigned)(psram_size/1024));

    if (s_psram_current) {
        free(s_psram_current);
        s_psram_current = NULL;
        s_psram_size = 0;
    }

    strlcpy(s_write_target, "PSRAM", sizeof(s_write_target));
    strlcpy(s_storage_mount, "PSRAM (heap_caps)", sizeof(s_storage_mount));

    ESP_LOGI(TAG, "Storage Backend: PSRAM (Volatile, lost on reboot) ⚠");
    return true;
}
#endif

#if STORAGE_BACKEND == 5
// ── SRAM Initialization ───────────────────────────────────────
static bool _init_sram(void) {
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free DRAM heap: %u KB", (unsigned)(free_heap/1024));

    if (s_sram_current) {
        free(s_sram_current);
        s_sram_current = NULL;
        s_sram_size = 0;
    }

    strlcpy(s_write_target, "SRAM", sizeof(s_write_target));
    strlcpy(s_storage_mount, "DRAM (malloc)", sizeof(s_storage_mount));

    ESP_LOGI(TAG, "Storage Backend: SRAM (Volatile, limited ~%u KB) ⚠",
             (unsigned)(free_heap/1024));
    return true;
}
#endif

// ═══════════════════════════════════════════════════════════════
//  STORAGE BACKEND: Initialize based on config
//  Called once at startup from rest_api_storage_init()
// ═══════════════════════════════════════════════════════════════
static bool _init_storage_backend(void) {
    #if STORAGE_BACKEND == 1
        return _init_littlefs();
    #elif STORAGE_BACKEND == 2
        return _init_spiffs();
    #elif STORAGE_BACKEND == 3
        return _init_sd_card();
    #elif STORAGE_BACKEND == 4
        return _init_psram();
    #elif STORAGE_BACKEND == 5
        return _init_sram();
    #else
        ESP_LOGE(TAG, "Invalid STORAGE_BACKEND value: %d", STORAGE_BACKEND);
        return false;
    #endif
}

// ═══════════════════════════════════════════════════════════════
//  STORAGE: Open file/RAM for writing (called before HTTP fetch)
//  Returns true on success
// ═══════════════════════════════════════════════════════════════
static bool _storage_open(int trip_id) {
    s_bytes_written = 0;

    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2
        snprintf(s_file_path, sizeof(s_file_path),
                 "%s%s/trips_%d.json",
                 s_storage_mount, STORAGE_DIR, trip_id);

        s_file_handle = fopen(s_file_path, "w");
        if (!s_file_handle) {
            ESP_LOGE(TAG, "Failed to open file for writing: %s", s_file_path);
            return false;
        }
        ESP_LOGI(TAG, "Storage file opened: %s", s_file_path);
        return true;

    #elif STORAGE_BACKEND == 3
        snprintf(s_file_path, sizeof(s_file_path),
                 "/sdcard%s/trips_%d.json",
                 STORAGE_DIR, trip_id);

        s_file_handle = fopen(s_file_path, "w");
        if (!s_file_handle) {
            ESP_LOGE(TAG, "Failed to open SD file for writing: %s", s_file_path);
            return false;
        }
        ESP_LOGI(TAG, "SD file opened: %s", s_file_path);
        return true;

    #elif STORAGE_BACKEND == 4
        if (s_psram_current) {
            free(s_psram_current);
            s_psram_current = NULL;
            s_psram_size = 0;
        }
        s_psram_current = (char *)heap_caps_malloc(TRIPS_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_psram_current) {
            ESP_LOGE(TAG, "PSRAM allocation failed for %d bytes", TRIPS_BUFFER_SIZE);
            return false;
        }
        s_psram_size = 0;
        s_psram_current[0] = '\0';
        ESP_LOGI(TAG, "PSRAM buffer allocated at %p (initial %d bytes)",
                 (void *)s_psram_current, TRIPS_BUFFER_SIZE);
        return true;

    #elif STORAGE_BACKEND == 5
        if (s_sram_current) {
            free(s_sram_current);
            s_sram_current = NULL;
            s_sram_size = 0;
        }
        s_sram_current = (char *)malloc(TRIPS_BUFFER_SIZE);
        if (!s_sram_current) {
            ESP_LOGE(TAG, "SRAM allocation FAILED — out of memory");
            return false;
        }
        s_sram_size = 0;
        s_sram_current[0] = '\0';
        ESP_LOGI(TAG, "SRAM buffer allocated at %p (initial %d bytes, free heap: %u KB)",
                 (void *)s_sram_current, TRIPS_BUFFER_SIZE,
                 (unsigned)(esp_get_free_heap_size()/1024));
        return true;
    #else
        return false;
    #endif
}

// ═══════════════════════════════════════════════════════════════
//  STORAGE: Append chunk of data (called during HTTP receive)
//  Returns bytes appended, or -1 on error
// ═══════════════════════════════════════════════════════════════
static int _storage_append(const char *data, int data_len) {
    if (data_len <= 0) return 0;

    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2 || STORAGE_BACKEND == 3
        if (!s_file_handle) return -1;
        size_t written = fwrite(data, 1, data_len, s_file_handle);
        if ((int)written != data_len) {
            ESP_LOGE(TAG, "File write error: requested %d, wrote %d", data_len, (int)written);
            return -1;
        }
        s_bytes_written += data_len;
        return data_len;

    #elif STORAGE_BACKEND == 4
        if (!s_psram_current) return -1;
        int new_size = s_psram_size + data_len + 1;  // +1 for null terminator
        char *new_buf = (char *)heap_caps_realloc(s_psram_current, new_size, MALLOC_CAP_SPIRAM);
        if (!new_buf) {
            ESP_LOGE(TAG, "PSRAM realloc FAILED at %d bytes total", new_size);
            return -1;
        }
        s_psram_current = new_buf;
        memcpy(s_psram_current + s_psram_size, data, data_len);
        s_psram_size += data_len;
        s_psram_current[s_psram_size] = '\0';
        s_bytes_written = s_psram_size;
        return data_len;

    #elif STORAGE_BACKEND == 5
        if (!s_sram_current) return -1;
        int new_sz = s_sram_size + data_len + 1;
        char *new_buf = (char *)realloc(s_sram_current, new_sz);
        if (!new_buf) {
            ESP_LOGE(TAG, "SRAM realloc FAILED at %d bytes (free heap: %u KB)",
                     new_sz, (unsigned)(esp_get_free_heap_size()/1024));
            return -1;
        }
        s_sram_current = new_buf;
        memcpy(s_sram_current + s_sram_size, data, data_len);
        s_sram_size += data_len;
        s_sram_current[s_sram_size] = '\0';
        s_bytes_written = s_sram_size;
        return data_len;
    #else
        return -1;
    #endif
}

// ═══════════════════════════════════════════════════════════════
//  STORAGE: Close file/finalize (called after HTTP fetch done)
// ═══════════════════════════════════════════════════════════════
static void _storage_close(void) {
    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2 || STORAGE_BACKEND == 3
        if (s_file_handle) {
            fclose(s_file_handle);
            s_file_handle = NULL;

            struct stat st;
            if (stat(s_file_path, &st) == 0) {
                ESP_LOGI(TAG, "File saved: %s | Size: %ld bytes (%.1f KB)",
                         s_file_path, (long)st.st_size, st.st_size / 1024.0f);
            }
        }
    #elif STORAGE_BACKEND == 4
        ESP_LOGI(TAG, "PSRAM buffer finalized | Total: %d bytes (%.1f KB)",
                 s_psram_size, s_psram_size / 1024.0f);
    #elif STORAGE_BACKEND == 5
        ESP_LOGI(TAG, "SRAM buffer finalized | Total: %d bytes (%.1f KB) | Free heap: %u KB",
                 s_sram_size, s_sram_size / 1024.0f,
                 (unsigned)(esp_get_free_heap_size()/1024));
    #endif

    ESP_LOGI(TAG, "─────────────── Storage Summary ───────────────");
    ESP_LOGI(TAG, "  Backend: %s", s_write_target);
    ESP_LOGI(TAG, "  Total written: %d bytes (%.1f KB)", s_bytes_written, s_bytes_written / 1024.0f);
    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2 || STORAGE_BACKEND == 3
        ESP_LOGI(TAG, "  Location: %s", s_file_path);
    #elif STORAGE_BACKEND == 4
        ESP_LOGI(TAG, "  Location: PSRAM @ %p", (void *)s_psram_current);
    #elif STORAGE_BACKEND == 5
        ESP_LOGI(TAG, "  Location: SRAM @ %p", (void *)s_sram_current);
    #endif
    ESP_LOGI(TAG, "  Permanence: %s",
             (STORAGE_BACKEND <= 3) ? "PERMANENT (survives power-off) ✓" : "TEMPORARY (lost on reboot) ⚠");
    ESP_LOGI(TAG, "────────────────────────────────────────────────");
}

// ═══════════════════════════════════════════════════════════════
//  HEAP DIAGNOSTICS — logged around every HTTPS call so a failure can be
//  told apart at a glance: TLS/memory failure (small "largest free block"
//  right before the call) vs. a server/auth-layer failure (heap numbers
//  look fine, HTTP actually completed).
// ═══════════════════════════════════════════════════════════════
static void _log_heap(const char *when) {
    size_t free_heap    = esp_get_free_heap_size();
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "  Heap %-18s free=%u KB | largest free block=%u KB",
             when, (unsigned)(free_heap / 1024), (unsigned)(largest_block / 1024));
}

// ═══════════════════════════════════════════════════════════════
//  HTTP EVENT HANDLER — called during HTTPS request
//  Writes response body chunks directly to storage backend
// ═══════════════════════════════════════════════════════════════
static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP error event");
            break;

        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP connected to %s", TRIPS_API_HOST);
            break;

        case HTTP_EVENT_HEADER_SENT:
            break;

        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "Response header: %.*s: %.*s",
                     (int)evt->data_len, (char *)evt->data,
                     (int)evt->data_len, (char *)evt->data);
            break;

        case HTTP_EVENT_ON_DATA:
            // ── THIS IS WHERE JSON IS STORED ─────────────────
            if (evt->data_len > 0) {
                int ret = _storage_append((const char *)evt->data, evt->data_len);
                if (ret < 0) {
                    ESP_LOGE(TAG, "Storage write FAILED at %d bytes", s_bytes_written);
                    return ESP_FAIL;
                }
                static int dot_counter = 0;
                dot_counter += evt->data_len;
                if (dot_counter >= 1024) {
                    printf(".");
                    dot_counter = 0;
                }
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            printf("\n");
            ESP_LOGI(TAG, "HTTP response finished");
            break;

        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP disconnected");
            break;

        default:
            break;
    }
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  FETCH TRIPS JSON from API and STORE it
// ═══════════════════════════════════════════════════════════════
static esp_err_t _fetch_and_store(int trip_id) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://%s%s/%d",
             TRIPS_API_HOST, TRIPS_API_PATH, trip_id);

    s_total_fetches++;

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "FETCH #%d → Trip ID: %d", s_total_fetches, trip_id);
    ESP_LOGI(TAG, "  URL: %s", url);
    ESP_LOGI(TAG, "  Storage backend: %s", s_write_target);
    ESP_LOGI(TAG, "──────────────────────────────────────");
    _log_heap("before HTTP client init");

    if (!_storage_open(trip_id)) {
        ESP_LOGE(TAG, "Storage open FAILED — aborting fetch");
        s_total_fails++;
        return ESP_FAIL;
    }

    esp_http_client_config_t http_cfg = {
        .url            = url,
        .method         = HTTP_METHOD_GET,
        .timeout_ms     = TRIPS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,  // TLS CA verification
        .event_handler  = _http_event_handler,
        .user_data      = NULL,
        .buffer_size    = TRIPS_BUFFER_SIZE,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init FAILED");
        _storage_close();
        s_total_fails++;
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ESP32-TaxiMeter/1.0");

    // API returns {"success":false,"message":"Job id not found in trips."}
    // for a request with no/invalid auth even when the trip ID is valid —
    // this header is required for real data. TRIPS_API_SEND_AUTH (config.h)
    // gates whether it's sent at all, so a failure can be isolated: does it
    // still fail the same way with auth removed entirely?
    #if TRIPS_API_SEND_AUTH
        char auth_header[700];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", TRIPS_API_BEARER_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth_header);
        ESP_LOGI(TAG, "  Authorization: Bearer %.12s...(redacted, %d chars total)",
                 TRIPS_API_BEARER_TOKEN, (int)strlen(TRIPS_API_BEARER_TOKEN));
    #else
        ESP_LOGW(TAG, "  Authorization: NOT sent (TRIPS_API_SEND_AUTH=0 in config.h)");
    #endif

    _log_heap("right before perform()");
    esp_err_t err = esp_http_client_perform(client);
    _log_heap("right after perform()");
    int status     = esp_http_client_get_status_code(client);
    int content_len = esp_http_client_get_content_length(client);

    if (err == ESP_OK) {
        s_total_ok++;
        ESP_LOGI(TAG, "✓ HTTP %d | Content-Length: %d | Bytes received: %d",
                 status, content_len, s_bytes_written);
        if (status != 200) {
            ESP_LOGW(TAG, "  Non-200 status with err=ESP_OK — server responded, but not with");
            ESP_LOGW(TAG, "  success. Check the response body below / 'api read %d'.", trip_id);
        }
    } else {
        s_total_fails++;
        ESP_LOGE(TAG, "✗ HTTP FAILED: %s (err=0x%04x)", esp_err_to_name(err), err);
        if (status > 0) {
            ESP_LOGE(TAG, "  HTTP status code: %d", status);
        }
        if (err == ESP_ERR_HTTP_CONNECT) {
            ESP_LOGE(TAG, "  ESP_ERR_HTTP_CONNECT with no status code usually means the TLS");
            ESP_LOGE(TAG, "  handshake itself failed (see esp-tls-mbedtls errors above, if any)");
            ESP_LOGE(TAG, "  rather than the server rejecting the request. Compare the 'largest");
            ESP_LOGE(TAG, "  free block' logged above against a healthy run to confirm.");
        }
    }

    _storage_close();

    // On success, print what actually came back — the API can return
    // HTTP 200 with a JSON body like {"success":false,"message":"..."}
    // for a bad trip ID or auth problem, which esp_http_client treats as
    // a perfectly fine transfer (err==ESP_OK, status==200).
    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2 || STORAGE_BACKEND == 3
        if (err == ESP_OK && s_bytes_written > 0) {
            FILE *preview_fp = fopen(s_file_path, "r");
            if (preview_fp) {
                char preview[201];
                size_t n = fread(preview, 1, sizeof(preview) - 1, preview_fp);
                preview[n] = '\0';
                ESP_LOGI(TAG, "  Response preview: %s%s", preview,
                         (size_t)s_bytes_written > n ? "..." : "");
                fclose(preview_fp);
            }
        }
    #endif

    // _storage_open() creates/opens the file BEFORE the HTTP call runs, so a
    // failed fetch (TLS/DNS/HTTP error, or a non-2xx status) still leaves a
    // 0-byte or partial file behind. Remove it here so only
    // successfully-fetched trips are ever stored or listed.
    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2 || STORAGE_BACKEND == 3
        if (err != ESP_OK && s_file_path[0] != '\0') {
            if (remove(s_file_path) == 0) {
                ESP_LOGW(TAG, "Removed incomplete file after failed fetch: %s", s_file_path);
            }
        }
    #endif

    ESP_LOGI(TAG, "Stats: %d fetches | %d OK | %d failed",
             s_total_fetches, s_total_ok, s_total_fails);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");

    esp_http_client_cleanup(client);
    return err;
}

// ═══════════════════════════════════════════════════════════════
//  READ stored JSON from file/RAM and print to Serial Monitor
// ═══════════════════════════════════════════════════════════════
static void _read_and_print(int trip_id) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "READ Trip ID: %d", trip_id);

    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2
        char path[256];
        snprintf(path, sizeof(path), "%s%s/trips_%d.json",
                 s_storage_mount, STORAGE_DIR, trip_id);

        struct stat st;
        if (stat(path, &st) != 0) {
            ESP_LOGE(TAG, "File NOT found: %s", path);
            ESP_LOGE(TAG, "Tip: Use 'api get %d' first to fetch and store.", trip_id);
            return;
        }

        FILE *fp = fopen(path, "r");
        if (!fp) {
            ESP_LOGE(TAG, "Failed to open file: %s", path);
            return;
        }

        ESP_LOGI(TAG, "──────────────────────────────────────");
        ESP_LOGI(TAG, "File: %s", path);
        ESP_LOGI(TAG, "Size: %ld bytes (%.1f KB)", (long)st.st_size, st.st_size / 1024.0f);
        ESP_LOGI(TAG, "──────────────────────────────────────");

        char *buf = (char *)malloc(MAX_PRINT_BYTES);
        if (buf) {
            size_t r = fread(buf, 1, MAX_PRINT_BYTES, fp);
            fwrite(buf, 1, r, stdout);
            if (r == MAX_PRINT_BYTES && (long)st.st_size > MAX_PRINT_BYTES) {
                printf("\n... (truncated at %d bytes, file is %ld bytes total)\n",
                       MAX_PRINT_BYTES, (long)st.st_size);
            }
            free(buf);
        }
        printf("\n");
        fclose(fp);

    #elif STORAGE_BACKEND == 3
        char sd_path[256];
        snprintf(sd_path, sizeof(sd_path), "/sdcard%s/trips_%d.json",
                 STORAGE_DIR, trip_id);

        struct stat st;
        if (stat(sd_path, &st) != 0) {
            ESP_LOGE(TAG, "SD file NOT found: %s", sd_path);
            ESP_LOGE(TAG, "Tip: Use 'api get %d' first to fetch and store.", trip_id);
            return;
        }

        FILE *fp = fopen(sd_path, "r");
        if (!fp) {
            ESP_LOGE(TAG, "Failed to open SD file: %s", sd_path);
            return;
        }

        ESP_LOGI(TAG, "──────────────────────────────────────");
        ESP_LOGI(TAG, "SD File: %s", sd_path);
        ESP_LOGI(TAG, "Size: %ld bytes (%.1f KB)", (long)st.st_size, st.st_size / 1024.0f);
        ESP_LOGI(TAG, "──────────────────────────────────────");

        char *buf = (char *)malloc(MAX_PRINT_BYTES);
        if (buf) {
            size_t r = fread(buf, 1, MAX_PRINT_BYTES, fp);
            fwrite(buf, 1, r, stdout);
            if (r == MAX_PRINT_BYTES && (long)st.st_size > MAX_PRINT_BYTES) {
                printf("\n... (truncated at %d bytes, file is %ld bytes total)\n",
                       MAX_PRINT_BYTES, (long)st.st_size);
            }
            free(buf);
        }
        printf("\n");
        fclose(fp);

    #elif STORAGE_BACKEND == 4
        if (!s_psram_current || s_psram_size == 0) {
            ESP_LOGW(TAG, "No data in PSRAM. Use 'api get <id>' first.");
            return;
        }
        ESP_LOGI(TAG, "──────────────────────────────────────");
        ESP_LOGI(TAG, "PSRAM buffer @ %p | Size: %d bytes (%.1f KB)",
                 (void *)s_psram_current, s_psram_size, s_psram_size / 1024.0f);
        ESP_LOGI(TAG, "──────────────────────────────────────");
        if (s_psram_size <= MAX_PRINT_BYTES) {
            printf("%s\n", s_psram_current);
        } else {
            printf("%.*s\n... (truncated at %d bytes, total: %d bytes)\n",
                   MAX_PRINT_BYTES, s_psram_current, MAX_PRINT_BYTES, s_psram_size);
        }

    #elif STORAGE_BACKEND == 5
        if (!s_sram_current || s_sram_size == 0) {
            ESP_LOGW(TAG, "No data in SRAM. Use 'api get <id>' first.");
            return;
        }
        ESP_LOGI(TAG, "──────────────────────────────────────");
        ESP_LOGI(TAG, "SRAM buffer @ %p | Size: %d bytes (%.1f KB)",
                 (void *)s_sram_current, s_sram_size, s_sram_size / 1024.0f);
        ESP_LOGI(TAG, "──────────────────────────────────────");
        if (s_sram_size <= MAX_PRINT_BYTES) {
            printf("%s\n", s_sram_current);
        } else {
            printf("%.*s\n... (truncated at %d bytes, total: %d bytes)\n",
                   MAX_PRINT_BYTES, s_sram_current, MAX_PRINT_BYTES, s_sram_size);
        }
    #endif

    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

// ═══════════════════════════════════════════════════════════════
//  LIST all stored JSON files with sizes
// ═══════════════════════════════════════════════════════════════
static void _list_files(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "LIST Stored JSON Files");

    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2 || STORAGE_BACKEND == 3
        char dir_path[128];
        #if STORAGE_BACKEND == 3
            snprintf(dir_path, sizeof(dir_path), "/sdcard%s", STORAGE_DIR);
        #else
            snprintf(dir_path, sizeof(dir_path), "%s%s", s_storage_mount, STORAGE_DIR);
        #endif

        DIR *dir = opendir(dir_path);
        if (!dir) {
            ESP_LOGW(TAG, "Cannot open directory: %s", dir_path);
            ESP_LOGW(TAG, "No files stored yet. Use 'api get <id>' first.");
            return;
        }

        ESP_LOGI(TAG, "──────────────────────────────────────");
        int count = 0;
        long total_bytes = 0;
        struct dirent *entry;

        while ((entry = readdir(dir)) != NULL && count < STORAGE_MAX_FILES) {
            if (entry->d_type != DT_REG) continue;
            char *name = entry->d_name;

            if (strncmp(name, "trips_", 6) != 0) continue;
            if (!strstr(name, ".json")) continue;

            char full_path[400];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);

            struct stat st;
            if (stat(full_path, &st) != 0) continue;

            count++;
            total_bytes += st.st_size;

            ESP_LOGI(TAG, "[%2d] %-30s %7.1f KB",
                     count, name, st.st_size / 1024.0f);
        }
        closedir(dir);

        ESP_LOGI(TAG, "──────────────────────────────────────");
        if (count == 0) {
            ESP_LOGI(TAG, "No JSON files found in %s", dir_path);
        } else {
            ESP_LOGI(TAG, "Total: %d file(s) | %.1f KB",
                     count, total_bytes / 1024.0f);
        }

    #elif STORAGE_BACKEND == 4
        ESP_LOGI(TAG, "──────────────────────────────────────");
        if (s_psram_current && s_psram_size > 0) {
            ESP_LOGI(TAG, "[ 1] PSRAM buffer     %7.1f KB  (volatile)",
                     s_psram_size / 1024.0f);
            ESP_LOGI(TAG, "Total: 1 entry (PSRAM holds 1 JSON at a time)");
        } else {
            ESP_LOGI(TAG, "PSRAM buffer is empty. Use 'api get <id>' first.");
        }

    #elif STORAGE_BACKEND == 5
        ESP_LOGI(TAG, "──────────────────────────────────────");
        if (s_sram_current && s_sram_size > 0) {
            ESP_LOGI(TAG, "[ 1] SRAM buffer      %7.1f KB  (volatile)",
                     s_sram_size / 1024.0f);
            ESP_LOGI(TAG, "Total: 1 entry (SRAM holds 1 JSON at a time)");
        } else {
            ESP_LOGI(TAG, "SRAM buffer is empty. Use 'api get <id>' first.");
        }
    #endif

    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

// ═══════════════════════════════════════════════════════════════
//  DELETE specific trip JSON file or ALL files
// ═══════════════════════════════════════════════════════════════
static void _delete_file(int trip_id, bool delete_all) {
    ESP_LOGI(TAG, "══════════════════════════════════════");

    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2 || STORAGE_BACKEND == 3
        char dir_path[128];
        #if STORAGE_BACKEND == 3
            snprintf(dir_path, sizeof(dir_path), "/sdcard%s", STORAGE_DIR);
        #else
            snprintf(dir_path, sizeof(dir_path), "%s%s", s_storage_mount, STORAGE_DIR);
        #endif

        if (delete_all) {
            ESP_LOGI(TAG, "DELETE ALL from: %s", dir_path);
            int deleted = 0;
            DIR *dir = opendir(dir_path);
            if (!dir) {
                ESP_LOGW(TAG, "Cannot open directory: %s", dir_path);
                return;
            }

            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_type != DT_REG) continue;
                if (strncmp(entry->d_name, "trips_", 6) != 0) continue;

                char full_path[400];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

                struct stat st;
                if (stat(full_path, &st) == 0) {
                    ESP_LOGI(TAG, "  Deleting: %s (%.1f KB)", entry->d_name, st.st_size / 1024.0f);
                }
                if (remove(full_path) == 0) {
                    deleted++;
                } else {
                    ESP_LOGE(TAG, "  FAILED to delete: %s", entry->d_name);
                }
            }
            closedir(dir);
            ESP_LOGI(TAG, "Deleted %d file(s) ✓", deleted);

        } else {
            char path[256];
            snprintf(path, sizeof(path), "%s/trips_%d.json", dir_path, trip_id);
            ESP_LOGI(TAG, "DELETE: %s", path);

            struct stat st;
            if (stat(path, &st) != 0) {
                ESP_LOGW(TAG, "File not found: %s", path);
                return;
            }

            ESP_LOGI(TAG, "  File size: %ld bytes (%.1f KB)", (long)st.st_size, st.st_size / 1024.0f);
            if (remove(path) == 0) {
                ESP_LOGI(TAG, "  Deleted successfully ✓");
            } else {
                ESP_LOGE(TAG, "  Delete FAILED ✗");
            }
        }

    #elif STORAGE_BACKEND == 4
        if (s_psram_current) {
            ESP_LOGI(TAG, "DELETE PSRAM buffer (%d bytes | %.1f KB)",
                     s_psram_size, s_psram_size / 1024.0f);
            free(s_psram_current);
            s_psram_current = NULL;
            s_psram_size = 0;
            ESP_LOGI(TAG, "PSRAM buffer freed ✓");
        } else {
            ESP_LOGW(TAG, "PSRAM buffer is already empty");
        }

    #elif STORAGE_BACKEND == 5
        if (s_sram_current) {
            ESP_LOGI(TAG, "DELETE SRAM buffer (%d bytes | %.1f KB)",
                     s_sram_size, s_sram_size / 1024.0f);
            free(s_sram_current);
            s_sram_current = NULL;
            s_sram_size = 0;
            ESP_LOGI(TAG, "SRAM buffer freed ✓ | Free heap: %u KB",
                     (unsigned)(esp_get_free_heap_size()/1024));
        } else {
            ESP_LOGW(TAG, "SRAM buffer is already empty");
        }
    #endif

    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

// ═══════════════════════════════════════════════════════════════
//  INFO — Show storage backend details, free space, stats
// ═══════════════════════════════════════════════════════════════
static void _show_info(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "STORAGE INFO");
    ESP_LOGI(TAG, "──────────────────────────────────────");

    ESP_LOGI(TAG, "  Backend:       %s (#%d)", s_write_target, STORAGE_BACKEND);
    ESP_LOGI(TAG, "  Mount:         %s", s_storage_mount);

    #if STORAGE_BACKEND == 1
        size_t total = 0, used = 0;
        if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "  LittleFS:      %u KB total | %u KB used | %u KB free",
                     (unsigned)(total/1024), (unsigned)(used/1024), (unsigned)((total-used)/1024));
        }
    #elif STORAGE_BACKEND == 2
        size_t total = 0, used = 0;
        if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "  SPIFFS:        %u KB total | %u KB used | %u KB free",
                     (unsigned)(total/1024), (unsigned)(used/1024), (unsigned)((total-used)/1024));
        }
    #elif STORAGE_BACKEND == 3
        if (s_sd_card) {
            ESP_LOGI(TAG, "  SD Card:       %llu MB total",
                     (unsigned long long)((uint64_t)s_sd_card->csd.capacity *
                     s_sd_card->csd.sector_size / (1024*1024)));
        }
    #elif STORAGE_BACKEND == 4
        ESP_LOGI(TAG, "  PSRAM total:   %u KB", (unsigned)(esp_psram_get_size()/1024));
        if (s_psram_current) {
            ESP_LOGI(TAG, "  PSRAM used:    %d bytes (%.1f KB)", s_psram_size, s_psram_size/1024.0f);
        } else {
            ESP_LOGI(TAG, "  PSRAM used:    0 (buffer empty)");
        }
    #elif STORAGE_BACKEND == 5
        ESP_LOGI(TAG, "  Free DRAM heap: %u KB", (unsigned)(esp_get_free_heap_size()/1024));
        if (s_sram_current) {
            ESP_LOGI(TAG, "  SRAM used:     %d bytes (%.1f KB)", s_sram_size, s_sram_size/1024.0f);
        } else {
            ESP_LOGI(TAG, "  SRAM used:     0 (buffer empty)");
        }
    #endif

    ESP_LOGI(TAG, "  Storage dir:   %s", STORAGE_DIR);
    ESP_LOGI(TAG, "  Buffer size:   %d bytes", TRIPS_BUFFER_SIZE);
    ESP_LOGI(TAG, "  HTTP timeout:  %d ms", TRIPS_HTTP_TIMEOUT_MS);

    const char *perm;
    #if STORAGE_BACKEND <= 3
        perm = "PERMANENT (Flash/SD — survives power-off)";
    #else
        perm = "TEMPORARY (RAM — lost on reboot) ⚠";
    #endif
    ESP_LOGI(TAG, "  Permanence:    %s", perm);

    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  Fetch stats:   %d total | %d OK | %d failed",
             s_total_fetches, s_total_ok, s_total_fails);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

// ═══════════════════════════════════════════════════════════════
//  HELP — Show all available serial commands with descriptions
// ═══════════════════════════════════════════════════════════════
static void _show_help(void) {
    printf("\n");
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "SERIAL COMMANDS — REST API Storage");
    ESP_LOGI(TAG, "──────────────────────────────────────");
    printf("  api get [id]      Fetch JSON from API and store\n");
    printf("                       id = trip ID (default: %d if omitted)\n", TRIPS_DEFAULT_ID);
    printf("                       Example: api get 12772\n");
    printf("\n");
    printf("  api read [id]     Read stored JSON and print to serial\n");
    printf("                       Example: api read 12772\n");
    printf("\n");
    printf("  api list          List all stored JSON files with sizes\n");
    printf("                       Example: api list\n");
    printf("\n");
    printf("  api delete <id>   Delete one specific trip JSON file\n");
    printf("                       Example: api delete 12772\n");
    printf("  api delete all    Delete ALL stored trip JSON files\n");
    printf("                       Example: api delete all\n");
    printf("\n");
    printf("  api info          Show storage backend, free space, stats\n");
    printf("                       Example: api info\n");
    printf("\n");
    printf("  api help          Show this help message\n");
    printf("\n");
    ESP_LOGI(TAG, "Storage backend: %s (#%d)", s_write_target, STORAGE_BACKEND);
    ESP_LOGI(TAG, "Default trip ID: %d", TRIPS_DEFAULT_ID);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

// ═══════════════════════════════════════════════════════════════
//  COMMAND PARSER — process "api <cmd> [arg]" input
//
//  Called from app_main.c's unified serial_cmd_task for every line
//  starting with "api ".
//
//  Returns true if command was recognized and processed
// ═══════════════════════════════════════════════════════════════
bool rest_api_storage_process_command(const char *line) {
    if (!line) return false;

    while (*line == ' ' || *line == '\t') line++;

    if (strncmp(line, "api ", 4) != 0 && strcmp(line, "api") != 0) return false;

    const char *cmd_start = line + 3;  // skip "api"
    while (*cmd_start == ' ') cmd_start++;

    if (*cmd_start == '\0') {
        _show_help();
        return true;
    }

    char cmd[16] = {0};
    const char *p = cmd_start;
    int ci = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && ci < 15) {
        cmd[ci++] = *p++;
    }
    cmd[ci] = '\0';

    while (*p == ' ') p++;
    const char *arg = p;

    char arg_clean[64] = {0};
    int ai = 0;
    while (*arg && *arg != '\r' && *arg != '\n' && ai < 63) {
        arg_clean[ai++] = *arg++;
    }
    arg_clean[ai] = '\0';

    int trip_id = TRIPS_DEFAULT_ID;
    if (arg_clean[0] != '\0') {
        if (strcmp(arg_clean, "all") == 0) {
            trip_id = -1;  // sentinel for "all"
        } else {
            trip_id = (int)strtol(arg_clean, NULL, 10);
            if (trip_id <= 0) {
                ESP_LOGW(TAG, "Invalid trip ID: '%s'. Using default: %d",
                         arg_clean, TRIPS_DEFAULT_ID);
                trip_id = TRIPS_DEFAULT_ID;
            }
        }
    }

    if (strcmp(cmd, "get") == 0) {
        ESP_LOGI(TAG, "▶ Command: FETCH trip ID %d", trip_id);
        if (!s_backend_ready) {
            ESP_LOGE(TAG, "Storage backend NOT ready. Check config.");
            return true;
        }
        // Routed through bg_worker (BG_JOB_TRIP_FETCH), NOT called
        // directly — this command handler runs on "serial_cmd" (limited
        // stack) when triggered via a typed/GUI command, and
        // _fetch_and_store()'s TLS handshake needs more stack depth than
        // that. bg_worker's own BG_JOB_TRIP_FETCH case (bg_worker.c)
        // already calls rest_api_storage_fetch(), which is this same
        // _fetch_and_store().
        if (!bg_worker_submit(BG_JOB_TRIP_FETCH, trip_id, NULL, NULL)) {
            ESP_LOGW(TAG, "api get: background worker busy — try again shortly");
        } else {
            ESP_LOGI(TAG, "api get: queued on background worker — watch below for progress");
        }
        return true;
    }

    if (strcmp(cmd, "read") == 0) {
        ESP_LOGI(TAG, "▶ Command: READ trip ID %d", trip_id);
        _read_and_print(trip_id);
        return true;
    }

    if (strcmp(cmd, "list") == 0) {
        ESP_LOGI(TAG, "▶ Command: LIST files");
        _list_files();
        return true;
    }

    if (strcmp(cmd, "delete") == 0 || strcmp(cmd, "del") == 0) {
        if (trip_id == -1) {
            ESP_LOGI(TAG, "▶ Command: DELETE ALL files");
            _delete_file(0, true);
        } else {
            ESP_LOGI(TAG, "▶ Command: DELETE trip ID %d", trip_id);
            _delete_file(trip_id, false);
        }
        return true;
    }

    if (strcmp(cmd, "info") == 0) {
        ESP_LOGI(TAG, "▶ Command: SHOW INFO");
        _show_info();
        return true;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        _show_help();
        return true;
    }

    ESP_LOGW(TAG, "Unknown command: '%s'. Type 'api help' for available commands.", cmd);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API — called from app_main()
//
//  rest_api_storage_init():
//    1. Check ENABLE_TRIPS_API flag
//    2. Mount/initialize storage backend
//    3. Log startup info
//
//  Does NOT create a serial task — app_main.c's single unified
//  serial_cmd_task calls rest_api_storage_process_command() directly.
//
//  Returns ESP_OK on success
// ═══════════════════════════════════════════════════════════════
esp_err_t rest_api_storage_init(void) {
    #if !ENABLE_TRIPS_API
        ESP_LOGI(TAG, "══════════════════════════════════════");
        ESP_LOGI(TAG, "TRIPS REST API: DISABLED — skipping");
        ESP_LOGI(TAG, "Set ENABLE_TRIPS_API=1 in config.h to enable");
        ESP_LOGI(TAG, "══════════════════════════════════════");
        return ESP_OK;
    #endif

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "TRIPS REST API + JSON Storage Init");
    ESP_LOGI(TAG, "  API: https://%s%s/{id}", TRIPS_API_HOST, TRIPS_API_PATH);
    ESP_LOGI(TAG, "  Default trip ID: %d", TRIPS_DEFAULT_ID);
    ESP_LOGI(TAG, "  Storage backend: %d", STORAGE_BACKEND);

    s_backend_ready = _init_storage_backend();
    if (!s_backend_ready) {
        ESP_LOGE(TAG, "Storage backend init FAILED — module will not work!");
        ESP_LOGE(TAG, "Check STORAGE_BACKEND config and partition table.");
        ESP_LOGE(TAG, "Serial commands will return errors until fixed.");
    }

    ESP_LOGI(TAG, "TRIPS API Storage — READY ✓");
    ESP_LOGI(TAG, "Type 'api help' for available commands.");
    ESP_LOGI(TAG, "══════════════════════════════════════\n");

    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API — code-callable (touchscreen UI)
//
//  These reuse the exact same internal functions the serial "api ..."
//  commands already call (_fetch_and_store, _delete_file, the same
//  file-naming/directory-scan logic as _list_files) — just returning
//  data to a caller instead of printing to the serial monitor.
// ═══════════════════════════════════════════════════════════════

esp_err_t rest_api_storage_fetch(int trip_id) {
    if (!s_backend_ready) {
        ESP_LOGE(TAG, "Storage backend NOT ready. Check config.");
        return ESP_ERR_INVALID_STATE;
    }
    return _fetch_and_store(trip_id);
}

int rest_api_storage_list(trip_file_info_t *out, int max_count) {
    if (!out || max_count <= 0) return 0;

    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2 || STORAGE_BACKEND == 3
        char dir_path[128];
        #if STORAGE_BACKEND == 3
            snprintf(dir_path, sizeof(dir_path), "/sdcard%s", STORAGE_DIR);
        #else
            snprintf(dir_path, sizeof(dir_path), "%s%s", s_storage_mount, STORAGE_DIR);
        #endif

        DIR *dir = opendir(dir_path);
        if (!dir) return 0;

        int count = 0;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && count < max_count) {
            if (entry->d_type != DT_REG) continue;
            const char *name = entry->d_name;
            if (strncmp(name, "trips_", 6) != 0) continue;
            if (!strstr(name, ".json")) continue;

            int id = atoi(name + 6);
            if (id <= 0) continue;

            char full_path[400];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);
            struct stat st;
            if (stat(full_path, &st) != 0) continue;

            out[count].trip_id    = id;
            out[count].size_bytes = (size_t)st.st_size;
            out[count].mtime      = st.st_mtime;
            count++;
        }
        closedir(dir);

        // Sort newest-modified first (simple insertion sort — count is small)
        for (int i = 1; i < count; i++) {
            trip_file_info_t key = out[i];
            int j = i - 1;
            while (j >= 0 && out[j].mtime < key.mtime) {
                out[j + 1] = out[j];
                j--;
            }
            out[j + 1] = key;
        }
        return count;
    #else
        // RAM backends (PSRAM/SRAM) hold at most one trip — not relevant
        // to the touchscreen trip-history list, which targets the SPIFFS
        // default (STORAGE_BACKEND=2).
        (void)out; (void)max_count;
        return 0;
    #endif
}

esp_err_t rest_api_storage_read(int trip_id, char **out_buf, size_t *out_len) {
    if (!out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_len = 0;

    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2
        char path[256];
        snprintf(path, sizeof(path), "%s%s/trips_%d.json", s_storage_mount, STORAGE_DIR, trip_id);

        struct stat st;
        if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;

        FILE *fp = fopen(path, "r");
        if (!fp) return ESP_FAIL;

        char *buf = (char *)malloc((size_t)st.st_size + 1);
        if (!buf) { fclose(fp); return ESP_ERR_NO_MEM; }

        size_t r = fread(buf, 1, (size_t)st.st_size, fp);
        buf[r] = '\0';
        fclose(fp);

        *out_buf = buf;
        *out_len = r;
        return ESP_OK;
    #else
        (void)trip_id;
        return ESP_ERR_NOT_SUPPORTED;
    #endif
}

esp_err_t rest_api_storage_delete(int trip_id) {
    _delete_file(trip_id, false);
    return ESP_OK;
}

esp_err_t rest_api_storage_write(int trip_id, const char *json, size_t len) {
    if (!json || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_backend_ready) {
        ESP_LOGE(TAG, "Storage backend NOT ready. Check config.");
        return ESP_ERR_INVALID_STATE;
    }

    #if STORAGE_BACKEND == 1 || STORAGE_BACKEND == 2
        char path[256];
        snprintf(path, sizeof(path), "%s%s/trips_%d.json", s_storage_mount, STORAGE_DIR, trip_id);

        FILE *fp = fopen(path, "w");
        if (!fp) {
            ESP_LOGE(TAG, "rest_api_storage_write: failed to open %s for writing", path);
            return ESP_FAIL;
        }
        size_t written = fwrite(json, 1, len, fp);
        fclose(fp);
        if (written != len) {
            ESP_LOGE(TAG, "rest_api_storage_write: short write (%u/%u bytes) for trip %d",
                     (unsigned)written, (unsigned)len, trip_id);
            remove(path);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "rest_api_storage_write: cached trip %d (%u bytes) -> %s", trip_id, (unsigned)len, path);
        return ESP_OK;
    #else
        (void)trip_id; (void)json; (void)len;
        return ESP_ERR_NOT_SUPPORTED;
    #endif
}
