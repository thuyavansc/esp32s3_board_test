#pragma once
// ================================================================
// hotspot_nvs.h — Persistent WiFi-hotspot credentials
//
// WHAT THIS MODULE DOES:
//   Pure storage — SSID/password/channel/max-clients/hidden-flag, NVS-
//   backed (survives reboot/OTA), cached in RAM for fast reads. Owns NO
//   WiFi API calls and NO serial commands of its own — hotspot_ap.c
//   calls into this for get/set and owns the actual SoftAP lifecycle +
//   the "hotspot ..." command family. Same separation-of-concerns this
//   project already uses for session_store.c (pure storage) vs.
//   auth_client.c (the actual login flow + its own commands).
//
// WHY NVS, NOT A SPIFFS FILE (doc 155 §5): small scalar values, this
// project already uses NVS for exactly this class of data
// (session_store.c/nvs_state.c), atomic per-key writes, wear-levelled —
// a hand-rolled SPIFFS file would be strictly worse here (no atomicity,
// corruptible mid-write) for no benefit.
//
// PASSWORD CHANGE SAFETY (your explicit requirement, doc 155 §5.2):
// hotspot_nvs_change_password() REQUIRES the correct CURRENT password
// before accepting a new one — a stolen/guessed serial/GUI session
// cannot silently take over the hotspot's credentials without already
// knowing them.
// ================================================================
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char    ssid[33];        // WiFi SSID — max 32 chars + NUL
    char    password[65];    // WPA2 password — max 63 chars + NUL ("" = open network, not recommended)
    uint8_t channel;         // 1-13
    uint8_t max_clients;
    bool    hidden;          // SSID broadcast suppressed if true
} hotspot_config_t;

// Loads from NVS, seeding config.h's HOTSPOT_DEFAULT_* on first boot
// (namespace never written before). Call once, early (session_store.c's
// own init ordering is the precedent — this is equally cheap/fast).
void hotspot_nvs_init(void);

// Current live config — a plain struct copy from the RAM cache, safe to
// call from any task, never touches NVS/flash directly.
void hotspot_nvs_get(hotspot_config_t *out);

// Change SSID only — persists immediately. No password gate (SSID isn't
// a secret) — hotspot_ap.c is responsible for restarting the AP after
// this call so the change actually takes effect.
esp_err_t hotspot_nvs_set_ssid(const char *ssid);

// Change password — validates old_password matches the CURRENTLY stored
// one first (returns ESP_ERR_INVALID_ARG if it doesn't — wrong-old-
// password is deliberately indistinguishable from "any other bad
// input" in the return code, so a caller can't use this as an oracle to
// brute-force the current password one guess at a time without also
// tripping whatever rate-limiting the caller layer applies). Validates
// new_password length is 8-63 chars (WPA2's real range — anything
// outside this would make the AP silently fail to start).
esp_err_t hotspot_nvs_change_password(const char *old_password, const char *new_password);

esp_err_t hotspot_nvs_set_channel(uint8_t channel);       // 1-13
esp_err_t hotspot_nvs_set_max_clients(uint8_t max_clients); // 1-10 (ESP_WIFI_MAX_CONN_NUM ceiling)
esp_err_t hotspot_nvs_set_hidden(bool hidden);

// Wipes hotspot NVS state back to config.h's factory defaults — the
// "I forgot the password" recovery path. Gated by the SAME
// FACTORY_RESET_PASSCODE the existing "factory reset" serial command
// already uses (config.h) — hotspot_ap.c's command handler enforces the
// passcode check before calling this; this function itself does the
// actual reset unconditionally once called (same "gate at the call
// site, not in the storage layer" pattern factory_reset.c already uses).
esp_err_t hotspot_nvs_reset_to_defaults(void);
