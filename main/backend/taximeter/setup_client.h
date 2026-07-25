#pragma once
// ================================================================
// setup_client.h — Network passcode + vehicle lookup
//
// WHAT THIS MODULE DOES:
//   GET devices-api/api/DevicePublic/Network?passcode=...  (Outcome<T>-
//     wrapped — {"success","message","data":{...}})
//   GET devices-api/api/DevicePublic/Vehicle?networkId=...&vehicleNo=...
//     (NOT wrapped — the server returns the VehicleDto directly, or a
//     bare 404/null if not found — a real asymmetry in the reference
//     API, not a bug here)
//
//   These are one-time-per-device provisioning steps — no UI screen
//   yet, run them from the serial monitor or let
//   setup_client_run_if_needed() do it automatically at boot using
//   config.h's SETUP_NETWORK_PASSCODE placeholder.
//
// SERIAL COMMANDS:
//   setup network [passcode]   → resolve network (default: config.h's
//                                  SETUP_NETWORK_PASSCODE)
//   setup vehicle [vehicleNo]  → resolve vehicle (default: config.h's
//                                  SETUP_VEHICLE_NO) — requires network
//                                  lookup to have already run this boot
//   setup info                 → show what's currently resolved
//   setup help                 → command list
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

esp_err_t setup_client_resolve_network(const char *passcode);
esp_err_t setup_client_resolve_vehicle(const char *vehicle_no);

// Runs both lookups using config.h's placeholders, but only if
// session_store doesn't already have a network_id/vehicle_id from a
// previous successful run — safe to call every boot.
void setup_client_run_if_needed(void);

bool setup_client_process_command(const char *line);
