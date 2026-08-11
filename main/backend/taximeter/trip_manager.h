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
#include <stddef.h>
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

// doc 184 §7.2 — a trip survives a reboot (its aggregate totals are
// persisted to SPIFFS), but the meter does NOT auto-resume it — the
// driver must confirm first, so nobody is surprised by a fare already
// running that they can't account for. trip_manager_init() detects a
// pending restore at boot; the UI should check
// trip_manager_has_pending_restore() once, right after login, and if
// true, show trip_manager_get_pending_restore_summary()'s message in a
// confirm dialog, then call trip_manager_confirm_restore() with the
// driver's answer.
bool trip_manager_has_pending_restore(void);

// Writes a driver-facing summary (e.g. "Trip #4 - $10.87 so far.\n
// Resume this trip?") into out. Safe to call even if there's nothing
// pending (writes an empty string).
void trip_manager_get_pending_restore_summary(char *out, size_t out_size);

// resume=true: restores fare_calc to the carried-forward state and
// keeps ticking normally from here. resume=false: restores just long
// enough to have the correct totals, then immediately stops and runs
// the full finalize/sync sequence (trip_manager_finalize_trip()) —
// whatever fare had genuinely accrued before the reboot still reaches
// the server; it's not silently discarded. Either way, clears the
// pending-restore state — call this at most once.
void trip_manager_confirm_restore(bool resume);

bool trip_manager_process_command(const char *line);
