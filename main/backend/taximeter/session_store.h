#pragma once
// ================================================================
// session_store.h — NVS-backed session/identity state
//
// WHAT THIS MODULE DOES:
//   Direct equivalent of the Android app's SharedPreferences-backed
//   UserDetails/VehicleDetails/NetworkDetails/ActiveTripDetails globals
//   (doc 78 §0, doc 80 §3) — a handful of small scalar values (token,
//   driver id, vehicle id, network id, current tariff type, duty
//   status) that every other module needs synchronous read access to.
//   Backed by ESP-IDF's NVS (non-volatile key-value flash storage,
//   already used for WiFi credentials via nvs_flash_init() in
//   app_main.c) — NOT SPIFFS/a database. This is the ESP32 analog of
//   SharedPreferences, chosen specifically because doc 80 §3 confirmed
//   it's a proven, direct match for this exact use case.
//
// THREADING:
//   All setters/getters open+close their own NVS handle per call — NVS
//   itself is safe to use from multiple tasks this way (it does its own
//   internal locking). Values are also cached in RAM (static struct) so
//   repeated reads (e.g. every fare-calc tick) don't touch flash at all
//   — only writes touch NVS.
//
// SERIAL COMMANDS:
//   session info    → dump every stored session field (see backend/
//                      taximeter/diag.c for the broader "mem"/"store"
//                      diagnostic commands this complements)
//   session clear   → wipe all session state (logout-equivalent reset)
// ================================================================
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Call once at boot, after nvs_flash_init(). Loads any previously-stored
// session state from flash into RAM; if this is the very first boot
// (nothing stored yet), seeds vehicle number from config.h's
// SETUP_VEHICLE_NO placeholder.
esp_err_t session_store_init(void);

// ── Auth/token state ──────────────────────────────────────────
void session_store_set_tokens(const char *access_token, const char *refresh_token, int64_t refresh_token_expiry_epoch);
bool session_store_get_access_token(char *out, size_t out_size);
bool session_store_get_refresh_token(char *out, size_t out_size);
int64_t session_store_get_access_token_expiry(void);   // epoch seconds, 0 if unknown
void session_store_set_access_token_expiry(int64_t epoch_seconds);
bool session_store_is_access_token_valid(void);         // false if missing/expired

// ── Identity state (driver/vehicle/network) ───────────────────
void session_store_set_driver(int64_t driver_id, const char *driver_no, const char *driver_name);
void session_store_set_vehicle(int64_t vehicle_id, int64_t vehicle_type_id, const char *vehicle_no);
void session_store_set_network(int64_t network_id, const char *company_name);
void session_store_set_tariff_type(const char *tariff_type);   // current Sedan/Maxi/etc. billing mode

int64_t session_store_get_driver_id(void);
int64_t session_store_get_vehicle_id(void);
int64_t session_store_get_vehicle_type_id(void);
int64_t session_store_get_network_id(void);
bool session_store_get_tariff_type(char *out, size_t out_size);
bool session_store_get_vehicle_no(char *out, size_t out_size);

// ── Duty status ─────────────────────────────────────────────────
typedef enum {
    DUTY_STATUS_OFF_DUTY = 0,
    DUTY_STATUS_ON_DUTY  = 1,
} duty_status_t;

void session_store_set_duty_status(duty_status_t status);
duty_status_t session_store_get_duty_status(void);

// ── Active trip (local id / server id) ────────────
// server_job_id <= 0 means "not yet synced to the server" — the exact
// analog of Android's nullable/<=0 serverEntityId sync-flag convention.
void session_store_set_active_trip(int32_t local_trip_id, int64_t server_job_id);
void session_store_clear_active_trip(void);
int32_t session_store_get_active_local_trip_id(void);   // 0 = no active trip
int64_t session_store_get_active_server_job_id(void);    // <=0 = not yet synced

// Reserve the next local trip id (a monotonically increasing counter
// persisted in NVS so it survives reboot and never repeats — the ESP32
// port rule for the local-id mechanism).
int32_t session_store_next_local_trip_id(void);

// Wipes every session field back to defaults (logout). Does NOT touch
// SETUP_VEHICLE_NO provisioning — that's per-device, not per-driver-session.
void session_store_clear(void);

// Prints every stored field to the serial log (backs the "session info"
// command) — this is how you see "what's actually stored" without guessing.
void session_store_print(void);

// Serial command handler ("session ...")
bool session_store_process_command(const char *line);
