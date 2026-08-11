#pragma once
// ================================================================
// hotspot_ap.h — WiFi SoftAP + NAPT internet-sharing lifecycle
//
// WHAT THIS MODULE DOES:
//   Owns the WiFi Access Point interface: start/stop, client list/kick,
//   and NAPT (NAT) so hotspot clients can reach whatever uplink
//   net_manager.c has chosen (cellular PPP or the WiFi STA connection).
//   Credentials come from hotspot_nvs.c (SSID/password/channel — this
//   module never touches NVS directly).
//
// DESIGN DECISION — why this needs ZERO changes to app_main.c's
// existing (proven, working) WiFi STA init:
//   esp_wifi_set_mode() is safe to call AFTER esp_wifi_start() to
//   promote WIFI_MODE_STA -> WIFI_MODE_APSTA at runtime, without
//   disturbing an already-connected STA session — this is standard,
//   documented ESP-IDF behavior. So hotspot_ap_start() does its own
//   esp_netif_create_default_wifi_ap() + esp_wifi_set_mode(APSTA) +
//   esp_wifi_set_config(WIFI_IF_AP,...) call, entirely independent of
//   however app_main.c already brought up STA. hotspot_ap_stop() calls
//   esp_wifi_set_mode(STA) to demote back down — the AP interface goes
//   away, STA is untouched. This keeps the hotspot fully self-
//   contained and never risks the already-working STA path.
//
// PERSISTED ON/OFF STATE (doc 169 — supersedes doc 155's original
// build-time-only design): hotspot_ap_init() reads hotspot_nvs.c's
// persisted "enabled" flag and auto-starts the AP if it was on last
// time — a hotspot you turn on stays on across a power cycle instead of
// reverting to off every boot. "hotspot on"/"hotspot off" (serial or
// GUI) are what change this persisted state going forward;
// config.h's HOTSPOT_DEFAULT_ENABLED only seeds a brand-new device's
// very first boot, before any NVS value has ever been written.
//
// SERIAL COMMANDS ("hotspot ..."):
//   hotspot on / off              Start/stop the AP at runtime
//   hotspot status                 SSID, channel, client count, NAPT state
//   hotspot ssid <name>            Change SSID (restarts AP if running)
//   hotspot passwd <old> <new>     Change password — old required
//   hotspot clients                Connected devices: MAC + IP + RSSI
//   hotspot kick <mac>              Disconnect one client (format AA:BB:CC:DD:EE:FF)
//   hotspot reset-credentials <passcode>   Factory-reset SSID/password
//                                            (same passcode as "factory reset")
//   hotspot help
// ================================================================
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

typedef struct {
    uint8_t        mac[6];
    esp_ip4_addr_t ip;    // .addr == 0 if the client hasn't completed DHCP yet
    int8_t         rssi;
} hotspot_client_t;

// Call once at boot, AFTER WiFi STA is already up (app_main.c's
// wifi_driver_bringup()) — registers the AP-side WiFi event handler,
// then auto-starts the AP if hotspot_nvs.c's persisted "enabled" flag
// says it was on last time; otherwise leaves it off until "hotspot on".
esp_err_t hotspot_ap_init(void);

// Starts the SoftAP with the current NVS-backed config (hotspot_nvs.c).
// Idempotent — a second call while already running is a no-op (logs a
// note, does not error).
esp_err_t hotspot_ap_start(void);

// Stops the SoftAP — demotes WiFi back to STA-only. STA connection
// (if any) is unaffected.
esp_err_t hotspot_ap_stop(void);

bool hotspot_ap_is_running(void);
esp_netif_t *hotspot_ap_get_netif(void);

// Called by net_manager.c once an uplink (cellular PPP or WiFi STA) has
// a real internet path — enables NAT so hotspot clients can reach it.
// Safe to call even if the hotspot isn't currently running (no-ops).
esp_err_t hotspot_ap_enable_napt(esp_netif_t *uplink_netif);
void hotspot_ap_disable_napt(void);

// Prepends the uplink's own DNS server (if it reports one — e.g. the
// carrier's DNS once PPP is up) ahead of the static HOTSPOT_DNS_*
// fallback, same "carrier DNS as primary, static as secondary" pattern
// the reference hotspot project uses (doc 15 §9.3).
void hotspot_ap_update_dns(esp_netif_t *uplink_netif);

int hotspot_ap_get_client_count(void);

// Copies up to max_count connected clients (MAC + IP + RSSI) into out.
// Returns the number actually written.
int hotspot_ap_get_clients(hotspot_client_t *out, int max_count);

// Disconnects one client by MAC address. Returns ESP_ERR_NOT_FOUND if
// that MAC isn't currently associated.
esp_err_t hotspot_ap_kick_client(const uint8_t mac[6]);

// Serial command handler ("hotspot ...")
bool hotspot_ap_process_command(const char *line);
