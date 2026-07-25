#pragma once
// ================================================================
// fare_calc.h — Live fare calculation
//
// WHAT THIS MODULE DOES:
//   The actual taxi-meter algorithm. Ticks every FARE_CALC_TICK_MS
//   (2000ms, matching the Android reference's TRIP_POINT_INTERVAL_MILLIS),
//   reads the latest GPS fix from gps_client, and decides — every tick —
//   whether to bill the tick's distance against the distance rate or the
//   tick's elapsed time against the time rate, based on the exact same
//   speed threshold the reference app uses (7.2 m/s / 25.9 km/h).
//
// WHAT'S DELIBERATELY SIMPLIFIED vs. the Android reference (intentional
// scope decisions, not bugs):
//   - No per-segment TripTimeFrame history array — distance tiering
//     only ever depends on the WHOLE-TRIP running odometer, so a
//     single running total is mathematically equivalent to summing
//     Android's per-segment fares for that purpose, without needing the
//     segment bookkeeping Android does for OTHER reasons (retroactive
//     re-rating on a mid-trip tariff switch, GPS-inactive
//     reconciliation). Both of those are later-phase features — this
//     module gives correct totals for the core drive-and-bill loop today.
//   - No retroactive GPS-inactive reconciliation — GPS-inactive seconds
//     are billed at the punitive rate as they happen and left as-is
//     when GPS returns (the simpler recommended option for ESP32).
//   - No manually-forced TIME/DISTANCE modes yet — only HYBRID (normal
//     speed-based billing) and PAUSED are implemented. Forced modes are
//     a small, self-contained addition for a later pass if needed.
//
// THREADING:
//   fare_calc_tick() must be called periodically from a task that is
//   NOT the LVGL thread (it does floating-point math and a GPS read —
//   cheap, but keep the separation clean; trip_manager.c owns the timer
//   that calls it). All fare_calc_get_snapshot() reads are safe to call
//   from the LVGL thread — it just copies out the current totals.
//
// SERIAL COMMANDS: none directly — controlled via "trip start/stop/pause"
// (backend/taximeter/trip_manager.c), which owns the fare_calc lifecycle.
// Use "trip info" to see live totals.
// ================================================================
#include <stdbool.h>
#include <stdint.h>
#include "reference_data.h"

#define FARE_CALC_TICK_MS  2000   // matches Android's TRIP_POINT_INTERVAL_MILLIS

// 7.2 m/s = 25.92 km/h — the exact speed threshold the reference app uses.
#define FARE_CALC_TIME_RATE_STOPPING_SPEED_MPS  7.2

// GPS-inactive seconds bill at this multiple of the normal time rate
// (the punitive-rate rule).
#define FARE_CALC_GPS_INACTIVE_MULTIPLIER  2.4

// GPS fix is trusted only if its reported accuracy is within this many
// meters — worse than this counts as "GPS inactive" for billing
// purposes even if a fix nominally exists.
#define FARE_CALC_GPS_ACCURACY_THRESHOLD_M  24.0

typedef struct {
    bool    is_running;
    bool    is_paused;
    bool    gps_active;

    double  distance_km;
    double  speed_kmh;              // instantaneous, from the latest GPS fix

    double  flag_fall_cents;
    double  distance_fare_cents;
    double  time_fare_cents;         // GPS-active time billing
    double  gps_inactive_fare_cents; // GPS-inactive (punitive-rate) time billing
    double  extras_cents;
    double  special_fares_cents;
    double  total_fare_cents;

    int64_t tariff_id;
    char    tariff_type[16];
    time_t  started_at;
} fare_calc_snapshot_t;

// Begin a new trip's fare calculation against the given tariff. Resets
// every running total to zero (except flag-fall, applied once here).
void fare_calc_start(const tariff_t *tariff);

// Stop — freezes the current totals (still readable via
// fare_calc_get_snapshot() afterward; call fare_calc_start() again to
// begin a fresh trip).
void fare_calc_stop(void);

void fare_calc_pause(void);
void fare_calc_resume(void);

// Call every FARE_CALC_TICK_MS from trip_manager's timer/task while a
// trip is running. No-op if not running or paused.
void fare_calc_tick(void);

// Add a flat amount (extras or a matched special fare) — cents, added
// directly to the respective running total.
void fare_calc_add_extras(double cents);
void fare_calc_add_special_fare_cents(double cents);

bool fare_calc_is_running(void);

// Copies the current state out — safe to call from any task.
void fare_calc_get_snapshot(fare_calc_snapshot_t *out);
