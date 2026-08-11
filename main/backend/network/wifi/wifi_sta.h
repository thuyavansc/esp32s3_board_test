#pragma once
// ================================================================
// wifi_sta.h — WiFi station (STA) connect/disconnect/scan, NVS-backed
//
// WHAT THIS MODULE DOES (doc 169 — replaces the OLD hardcoded/blocking
// design in app_main.c):
//   Owns the "should I even try to connect, and to what" decision that
//   used to be a compile-time SSID/password (config.h's WIFI_SSID/
//   WIFI_PASS) plus a boot that BLOCKED FOREVER until that exact network
//   was found. Neither is viable in production: the WiFi network (if
//   any) available at a given deployed vehicle isn't known at build
//   time, and a missing/out-of-range network must never hang the whole
//   device at boot — it should just carry on (cellular is the intended
//   primary uplink anyway; see net_manager.c).
//
// NAMING: every public function here is prefixed "app_wifi_", NOT
// "wifi_sta_" — ESP-IDF's own internal WiFi driver library
// (libnet80211.a) turned out to already define a global C symbol
// called exactly "wifi_sta_disconnect" (an internal, undocumented
// function, not part of the public esp_wifi.h API) — the linker
// correctly rejected the resulting duplicate-symbol collision. Kept the
// file/struct names as "wifi_sta_*" (types don't get linked, no
// conflict there) but every FUNCTION got the more distinctive prefix to
// avoid this whole class of collision with ESP-IDF's own internal,
// non-namespaced C symbols.
//
// STORAGE: same "static in-RAM cache, mirrored to NVS on every write"
// pattern hotspot_nvs.c uses — folded into this ONE file rather than a
// separate wifi_nvs.c, since (unlike hotspot) nothing else in the
// firmware needs to read this storage independently of the connect/
// disconnect logic itself.
//
// THE "DON'T AUTO-RECONNECT" PROBLEM THIS FIXES: the old code's
// WIFI_EVENT_STA_DISCONNECTED handler called esp_wifi_connect() again
// UNCONDITIONALLY, forever, with no way to actually stay disconnected —
// there was no disconnect command at all. app_wifi_disconnect() now
// clears an internal "want connected" flag that the SAME disconnect
// handler checks before ever reconnecting on its own.
//
// SERIAL COMMANDS ("wifi ..."):
//   wifi scan                          Scan for nearby networks
//   wifi connect <ssid> <password>     Connect (persists to NVS, enables auto-connect)
//   wifi disconnect                    Disconnect now, don't auto-reconnect
//   wifi autoconnect on|off            Persisted preference (boot-time + auto-reconnect-after-drop)
//   wifi status                        Connected? SSID, IP, signal, auto-connect state
//   wifi help
// ================================================================
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool    connected;
    char    ssid[33];
    char    ip[16];
    int8_t  rssi;          // dBm, only meaningful if connected
    bool    auto_connect;  // persisted preference — not the same as "connected right now"
} wifi_sta_status_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t auth_mode;     // wifi_auth_mode_t value (0 = WIFI_AUTH_OPEN)
} wifi_scan_result_t;

// Call once, right after esp_wifi_start() in app_main.c's boot sequence
// (needs the WiFi driver already up, STA mode set). Loads NVS config,
// seeding from config.h's WIFI_SSID/WIFI_PASS/WIFI_STA_DEFAULT_AUTO_CONNECT
// on a brand-new device's very first boot only. If auto-connect is on
// and an SSID is stored, attempts ONE BOUNDED connect
// (WIFI_STA_CONNECT_TIMEOUT_MS, config.h) — returns either way, never
// blocks boot indefinitely like the old design did.
void app_wifi_init(void);

// Runtime connect — persists the new SSID/password to NVS, sets
// auto_connect=true (persisted), disconnects from whatever's currently
// associated (if anything), and connects to the new network. Blocks the
// calling task up to WIFI_STA_CONNECT_TIMEOUT_MS — route through
// bg_worker_submit_fn() if calling from LVGL/a small-stack task.
// Returns ESP_ERR_TIMEOUT (not a hard failure) if the timeout expires —
// the disconnect-handler keeps retrying in the background regardless.
esp_err_t app_wifi_connect(const char *ssid, const char *password);

// Disconnects the CURRENT session and stops the auto-reconnect-on-drop
// behavior until told otherwise (does NOT clear the persisted SSID/
// password — use app_wifi_set_auto_connect(false) to also stop
// reconnecting after the next reboot).
void app_wifi_disconnect(void);

// Persists the auto-connect preference on its own. Turning it back on
// while a valid SSID is stored also triggers an immediate reconnect
// attempt (does not wait for the next boot/drop).
void app_wifi_set_auto_connect(bool enabled);

// Blocking scan (a few seconds, ESP-IDF's own active-scan default) —
// fills out[] with up to max_count found networks. Do not call from the
// LVGL thread — route through bg_worker_submit_fn().
int app_wifi_scan(wifi_scan_result_t *out, int max_count);

void app_wifi_get_status(wifi_sta_status_t *out);

// Serial command handler ("wifi ...").
bool app_wifi_process_command(const char *line);
