#pragma once
// ================================================================
// net_diag.h — WiFi status + real internet-reachability check.
//
// WHY THIS IS SEPARATE FROM api_client.c:
//   api_client.c answers "is the TaxiMeter backend reachable, with a
//   valid auth token?" — this module answers a more basic, more
//   general question: "does this device have ANY working path to the
//   internet right now?", independent of the TaxiMeter backend, so a
//   login failure can be isolated to "no internet at all" vs. "internet
//   works, backend/app-version/credentials problem" (see doc 141).
//
// SERIAL COMMANDS:
//   net info   → own STA MAC, connected SSID/BSSID/RSSI/channel, local
//                 IP/netmask/gateway/DNS — whatever WiFi/esp_netif know
//                 right now, no network traffic generated
//   net test   → a REAL HTTPS GET against a public "connectivity check"
//                 endpoint — the same technique phones/laptops use to
//                 tell "associated to an AP" apart from "actually has
//                 working internet" (DNS + TCP + TLS all have to
//                 succeed, not just an ARP/link check) — reports HTTP
//                 status + round-trip time, not just a boolean
//   net help
// ================================================================
#include <stdbool.h>

bool net_diag_process_command(const char *line);
