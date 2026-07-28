#pragma once
// ================================================================
// net_manager.h — Uplink selection (WiFi <-> Cellular)
//
// WHAT THIS MODULE DOES:
//   The one thing in this project that knows about BOTH cellular_ppp.c
//   and hotspot_ap.c (neither of those two knows about the other, or
//   about this module — clean, one-directional layering). Decides
//   which network interface is the system's "default route"
//   (esp_netif_set_default_netif() — used both by the ESP32's own
//   outbound HTTPS calls, e.g. api_client.c, AND by hotspot NAPT
//   routing), and re-points NAPT + DNS whenever that changes — exactly
//   like a phone switching between WiFi and cellular data, per your
//   requirement.
//
// BOOT BEHAVIOR: does not block app_main()'s startup. WiFi-STA is
// already connected by the time this runs (app_main.c's existing
// blocking wifi_init_and_wait()), so net_manager_init() sets that as
// the immediate default, then — if NET_UPLINK_PREFER_CELLULAR
// (config.h) — queues a background job (via bg_worker, same pattern
// every other HTTPS-capable module in this project uses) that dials
// cellular and promotes it to the active uplink once/if it connects.
//
// SERIAL COMMANDS: dispatched from net_diag.c's EXISTING "net ..."
// handler (not a second "net" prefix owner — see net_diag.c's own
// comment on why) — net_diag.c calls straight into this module's plain
// functions below for "net uplink ...", "net status", "net mem".
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    NET_UPLINK_NONE     = 0,   // no uplink chosen/available yet
    NET_UPLINK_WIFI     = 1,
    NET_UPLINK_CELLULAR = 2,
} net_uplink_t;

typedef struct {
    net_uplink_t active_uplink;
    bool wifi_connected;
    bool cellular_connected;
    bool hotspot_running;
    bool napt_active;
} net_status_t;

// Call once at boot, AFTER WiFi STA, cellular_ppp_init(), AND
// hotspot_ap_init() have all already run. Non-blocking (see header
// comment) — the actual cellular dial-up (if preferred) happens on a
// background job.
esp_err_t net_manager_init(void);

net_uplink_t net_manager_get_active_uplink(void);

// Explicit switch — BLOCKS the calling task if switching TO cellular
// and it isn't already connected (dials, up to ~30s). Callers on a
// small-stack task (serial console) MUST route this through
// bg_worker_submit_fn(), same rule as every other HTTPS-capable call in
// this project (see fare_calc.h's threading note for the precedent).
esp_err_t net_manager_set_uplink(net_uplink_t uplink);

// "Prefer cellular, fall back to WiFi if cellular fails" — same
// blocking/bg_worker rule as net_manager_set_uplink() above.
esp_err_t net_manager_set_uplink_auto(void);

void net_manager_get_status(net_status_t *out);

// Human-readable dump of the full picture (uplink, WiFi, cellular,
// hotspot, NAPT) — used by "net uplink"/"net status" and reusable by
// any future GUI code that wants the exact same summary.
void net_manager_print_status(void);
