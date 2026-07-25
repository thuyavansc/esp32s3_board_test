#pragma once
// ================================================================
// trip_manager.h — Local trip lifecycle
//
// WHAT THIS MODULE DOES:
//   Ties together session_store (local/server trip id — offline-first
//   sync-flag mechanism), reference_data (tariff lookup), fare_calc
//   (the live meter), and gps_client (pickup location) into the actual
//   "start trip" / "stop trip" driver actions.
//
//   Owns the one periodic task that ticks the meter every
//   FARE_CALC_TICK_MS (2s) while a trip is running, and persists trip
//   state to SPIFFS so an in-progress trip survives a reboot.
//
// SCOPE — what this does NOT do yet (deliberately deferred):
//   - No server sync yet (AddJobByDriver / Trips / SaveJobFares). A trip
//     started here stays fully local — session_store's server_job_id
//     stays 0. The local-id/offline-first groundwork is all in place;
//     wiring the actual sync calls is a separate, later pass.
//   - No live toll/geofence detection — only the flat "Levy"
//     special-fare auto-add is implemented, since that's a simple
//     unconditional lookup, not geofencing.
//   - No mid-trip Sedan/Maxi tariff-type switch yet — the tariff is
//     resolved once, at trip start.
//
// SERIAL COMMANDS:
//   trip start [customerName]   → resolve tariff, start the meter
//   trip stop                    → stop the meter
//   trip pause / trip resume     → pause/resume billing
//   trip extras <amount>         → add a flat extras charge (dollars)
//   trip info                    → live fare breakdown
//   trip help
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

// Call once at boot — starts the periodic fare-calc tick task.
void trip_manager_init(void);

// Starts a new local trip: reserves a local trip id, captures the
// current GPS fix as pickup location, resolves the active tariff via
// reference_data_find_tariff_by_time(), starts fare_calc, auto-adds the
// "Levy" special fare if the reference data has one, and persists
// initial trip state to SPIFFS.
esp_err_t trip_manager_start_trip(const char *customer_name);

esp_err_t trip_manager_stop_trip(void);
void trip_manager_pause_trip(void);
void trip_manager_resume_trip(void);

// Extras amount is in the SAME unit as tariff rates (cents) — callers
// passing dollars should multiply by 100 first.
void trip_manager_add_extras_cents(double cents);

bool trip_manager_is_trip_active(void);

bool trip_manager_process_command(const char *line);
