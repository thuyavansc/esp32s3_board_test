#pragma once
// ================================================================
// rest_api_storage.h — HTTPS REST API Trips Client + JSON Storage
//
// WHAT THIS MODULE DOES:
//   1. Fetches trips JSON from:
//      https://mytaxis.softclient.com.au/taxis-api/api/Trips/{id}
//      using ESP-IDF esp_http_client with TLS (cert bundle)
//   2. Stores the JSON response to the selected storage backend:
//      - LittleFS (Flash filesystem — permanent, recommended)
//      - SPIFFS   (Legacy Flash filesystem — this project's default)
//      - SD Card  (External, removable, huge capacity)
//      - PSRAM    (Extra RAM on ESP32-S3, volatile)
//      - SRAM     (Internal RAM buffer, volatile, limited)
//   3. Reads back stored JSON files and prints to Serial Monitor
//   4. Provides Serial Monitor commands for complete control:
//      - api get <trip_id>     → Fetch from API and store
//      - api read <trip_id>    → Read stored JSON and print
//      - api list              → List all stored files with sizes
//      - api delete <trip_id>  → Delete one specific file
//      - api delete all        → Delete all stored JSON files
//      - api info              → Show storage backend, free/total space
//      - api help              → Show all available commands
//
// FLOW (3 stages):
//
//   STAGE 1 — FETCH JSON:
//     Serial command "api get <id>"
//       → Build URL: https://mytaxis.softclient.com.au/taxis-api/api/Trips/<id>
//       → HTTP GET with TLS (esp_http_client + crt_bundle)
//       → Stream response in TRIPS_BUFFER_SIZE chunks
//       → Write chunks to selected storage backend
//       → Log: status code, content length, storage location, file size
//
//   STAGE 2 — STORE JSON:
//     Based on STORAGE_BACKEND config:
//       LittleFS  → fopen("/store/trips_<id>.json", "w") → fwrite chunks → fclose
//       SPIFFS    → Same file I/O as LittleFS (different mount point)
//       SD Card   → Same file I/O (different mount point)
//       PSRAM     → malloc() in PSRAM → memcpy chunks → pointer stored
//       SRAM      → malloc() in DRAM  → memcpy chunks → pointer stored
//
//     For RAM backends (PSRAM/SRAM):
//       Only ONE JSON can be stored at a time (current_ptr is overwritten).
//       RAM is volatile — data lost on reboot.
//       File backends (LittleFS/SPIFFS/SD) store unlimited files.
//
//   STAGE 3 — SERIAL COMMAND PARSE:
//     app_main.c's unified serial task reads UART input line-by-line
//     and passes any "api " prefixed line here (this module does NOT
//     run its own serial task — see rest_api_storage_init()).
//
// FILE FORMAT ON STORAGE:
//   /store/trips_<id>.json         ← Raw JSON, exactly as received
//
// DEPENDENCIES:
//   config.h        → ENABLE_TRIPS_API, STORAGE_BACKEND, TRIPS_API_HOST, etc.
//   esp_http_client → HTTPS GET with TLS cert bundle
//   esp_spiffs      → STORAGE_BACKEND == 2 (this project's default)
//
// ENABLE/DISABLE:
//   Set ENABLE_TRIPS_API = 1 or 0 in config.h
//   When 0: module initializes but does nothing, no code compiled.
//
// ================================================================
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

// Init the REST API + storage module (call ONCE after WiFi connects in
// app_main). Mounts the storage backend (SPIFFS by default —
// STORAGE_BACKEND in config.h) — does NOT start its own serial task;
// app_main.c's single unified serial_cmd_task calls
// rest_api_storage_process_command() directly for any "api ..." line.
esp_err_t rest_api_storage_init(void);

// Process a serial command string ("api ...")
// Returns true if command was recognized and processed
bool rest_api_storage_process_command(const char *line);

// ── Code-callable API (for the touchscreen UI — NOT via serial commands) ──
// Each of these reuses the exact logic the serial "api ..." commands already
// use internally — just returns data to a caller instead of printing to UART.

typedef struct {
    int    trip_id;
    size_t size_bytes;
    time_t mtime;
} trip_file_info_t;

// Fetch trip JSON from the API and store it (same as "api get <id>").
// Blocks on the HTTPS request — call from a background task, never the LVGL thread.
esp_err_t rest_api_storage_fetch(int trip_id);

// List stored trips, newest-modified first. Returns the number of entries
// written into `out` (up to max_count).
int rest_api_storage_list(trip_file_info_t *out, int max_count);

// Read a stored trip's raw JSON into a newly malloc'd buffer (caller must
// free() it). Returns ESP_ERR_NOT_FOUND if that trip isn't stored.
esp_err_t rest_api_storage_read(int trip_id, char **out_buf, size_t *out_len);

// doc 188: write a JSON blob directly into the trips_<id>.json convention,
// for caching one record out of a Job/GetAllBySearch history-list response
// (which already returns full JobDto bodies per row — no need for a
// separate per-trip GET /Trips/{id} the way rest_api_storage_fetch() does).
// Same backend scope as rest_api_storage_read() (STORAGE_BACKEND 1/2 only
// — the file backends). RAM backends return ESP_ERR_NOT_SUPPORTED, same
// as read's existing precedent, since they only ever hold one JSON at a
// time and aren't used for the touchscreen trip-history list.
esp_err_t rest_api_storage_write(int trip_id, const char *json, size_t len);

// Delete a stored trip's JSON file (same as "api delete <id>").
esp_err_t rest_api_storage_delete(int trip_id);
