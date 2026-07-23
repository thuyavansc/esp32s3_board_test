#pragma once
// ================================================================
// nvs_state.h — persistent OTA/device state, added 2026-07-13 (doc 52)
//
// Everything here answers doc 52 §5's "what goes in NVS" question
// (docs/TestFunctionalities/ota-updates/52_2026-07-13_company_dotnet_backend_comparison_analysis.md):
// small, structured, frequently-updated values that must survive a
// reboot — including the reboot an OTA update itself causes. This is
// deliberately NOT SPIFFS (trips_api.c owns SPIFFS, for large trip-JSON
// blobs — a different access pattern entirely) and NOT compiled-in
// constants (version.h's FW_* defines describe THIS BINARY; the values
// here describe THIS PHYSICAL DEVICE's runtime state, which can't be
// baked into the firmware image).
//
// Namespace: "ota_state" — kept separate from any other NVS usage in
// this project so a future module can't accidentally collide with these
// keys.
// ================================================================
#include <stdbool.h>
#include <stddef.h>

// Call once, early in app_main() (after nvs_flash_init()). Idempotent.
void nvs_state_init(void);

// ── Provisioned identity ────────────────────────────────────────
// Defaults to OTA_DEFAULT_COMPANY_ID / OTA_DEFAULT_PRODUCT_NAME
// (config.h) until explicitly set via "nvs company <id>" / "nvs product
// <name>" over serial — see nvs_state_process_command() below.
void nvs_state_get_company(char *out, size_t out_len);
void nvs_state_set_company(const char *company_id);
void nvs_state_get_product(char *out, size_t out_len);
void nvs_state_set_product(const char *product_name);

// ── Remote configuration (added 2026-07-14) ─────────────────────
// Fetched by remote_config.c from the backend and written here; falls
// back to compiled config.h defaults (usually "") when never configured
// by an admin yet — same fallback pattern as company/product above.
// server_url is the general base server URL — see config.h's
// ENABLE_SERVER_URL_OVERRIDE for how/whether it's actually used to
// redirect where OTA/remote-config themselves connect.
void nvs_state_get_taxi_number(char *out, size_t out_len);
void nvs_state_set_taxi_number(const char *v);
void nvs_state_get_sms_number(char *out, size_t out_len);
void nvs_state_set_sms_number(const char *v);
void nvs_state_get_emergency_number(char *out, size_t out_len);
void nvs_state_set_emergency_number(const char *v);
void nvs_state_get_server_url(char *out, size_t out_len); // "" if never set
void nvs_state_set_server_url(const char *v);
void nvs_state_clear_server_url(void); // factory_reset.c's safety net

// Resolves the EFFECTIVE server to connect to: if ENABLE_SERVER_URL_OVERRIDE
// (config.h) is on AND an admin has set a serverUrl via remote config,
// parses that ("https://host[:port]") into host/port/use_https. Otherwise
// (flag off, or NVS empty — fresh device / feature not yet used) returns
// today's exact compiled OTA_SERVER_HOST/OTA_SERVER_PORT/OTA_USE_HTTPS
// unchanged. ota_client.c and remote_config.c both call this instead of
// reading the OTA_SERVER_* macros directly, so both consistently honor a
// remote override the same way.
void nvs_state_resolve_server(char *host_out, size_t host_out_len, int *port_out, bool *use_https_out);

// ── Update-in-progress tracking ─────────────────────────────────
// Written by ota_client.c immediately before esp_https_ota_finish() +
// esp_restart() — survives the reboot so the post-boot self-test (see
// ota_client_init()) knows WHICH release it's verifying, since
// ESP-IDF's own ESP_OTA_IMG_PENDING_VERIFY state doesn't carry that
// information (only that SOME image is pending confirmation).
void nvs_state_set_pending(int version_code, const char *firmware_id);
// Returns true and fills the outputs if a pending update is recorded.
bool nvs_state_get_pending(int *version_code_out, char *firmware_id_out, size_t firmware_id_len);
void nvs_state_clear_pending(void);

// ── Update-loop protection — SUPERSEDED, NOT deleted (doc 60, 2026-07-13) ──
// ota_client.c no longer calls any of these three. Originally: tracked a
// consecutive-failure streak per versionCode IN NVS, surviving reboots,
// so a version that failed enough times got permanently skipped until a
// newer one appeared. Explicitly replaced with a RAM-only, resets-every-
// boot immediate-retry approach instead (config.h's
// OTA_MANIFEST_RETRY_COUNT/OTA_DOWNLOAD_RETRY_COUNT, ota_client.c) — the
// old NVS-persisted version was blocking a device from retrying a
// versionCode that had already failed BEFORE a bugfix was flashed, even
// though the bug was already fixed. Left here, still fully functional,
// in case NVS-persisted update-loop protection is wanted again later.
void nvs_state_record_failure(int version_code);
void nvs_state_clear_failure(void);
bool nvs_state_should_skip(int version_code, int max_retries);

// ── Diagnostics ──────────────────────────────────────────────────
// Parses "nvs status" / "nvs company <id>" / "nvs product <name>" from
// the serial command reader. Returns true if recognized.
bool nvs_state_process_command(const char *line);
