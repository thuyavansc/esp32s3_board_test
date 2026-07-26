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
//   - Server sync now wired (D5, doc 151 §7.1: built before the fare-
//     accuracy pass) via trip_sync.c — trip_manager_start_trip() queues
//     an AddJob as soon as the meter starts, the tick loop queues a
//     periodic Trips-update (TRIP_SYNC_EVERY_N_TICKS below) once a
//     server job id exists, and trip_manager_finalize_trip() runs the
//     full AddJob->Trips->SaveJobFares sequence. See trip_sync.h for
//     what's still explicitly NOT implemented within sync itself
//     (Pickup/ChangeStatus, structured special-fare/toll breakdowns).
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
//   trip finalize                 → stop (if not already), then run the
//                                   full AddJob/Trips/SaveJobFares sync
//                                   sequence and clear the active trip
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
// "Levy" special fare if the reference data has one, persists initial
// trip state to SPIFFS, and queues a background AddJob sync (fire-and-
// forget — matches Android's own StartTripUseCase calling
// tripSyncManager.requestSync() right after starting, doc 149 §5).
esp_err_t trip_manager_start_trip(const char *customer_name);

esp_err_t trip_manager_stop_trip(void);
void trip_manager_pause_trip(void);
void trip_manager_resume_trip(void);

// Stops the meter (if not already stopped), then runs the full
// AddJob->Trips->SaveJobFares sequence (trip_sync_run_full_sequence())
// on the background worker task, and — only on success — clears the
// active trip so the next "trip start" reserves a fresh local id. On
// failure, the active trip id is deliberately kept so a later
// "sync now" (or the next "trip finalize") can resume from wherever it
// left off, matching Android's own per-step resumability (doc 149 §2.2).
esp_err_t trip_manager_finalize_trip(void);

// Extras amount is in the SAME unit as tariff rates (cents) — callers
// passing dollars should multiply by 100 first.
void trip_manager_add_extras_cents(double cents);

bool trip_manager_is_trip_active(void);

bool trip_manager_process_command(const char *line);
