#pragma once
// ================================================================
// fare_calc.h — Live fare calculation + per-segment TimeFrame history
//
// WHAT THIS MODULE DOES:
//   The actual taxi-meter algorithm. Ticks every FARE_CALC_TICK_MS
//   (2000ms, matching the Android reference's TRIP_POINT_INTERVAL_MILLIS),
//   reads the latest GPS fix from gps_client, and decides — every tick —
//   whether to bill the tick's distance against the distance rate or the
//   tick's elapsed time against the time rate, based on the exact same
//   speed threshold the reference app uses (7.2 m/s / 25.9 km/h).
//
//   Trip history is kept as a SEQUENCE of TimeFrame segments (doc
//   docs/TestFunctionalities/esp32s3_board/calculations-impl/
//   150_2026-07-26_android_fare_calculation_timeframe_logic_and_
//   decision_tree.md §0) — a new segment opens every time the billing
//   mode changes (GPS active<->inactive, or paused<->resumed), mirroring
//   Android's TripTimeFrame table exactly (D3, doc 151 §7.1: full
//   per-segment history, not a single running total — chosen so the
//   trip_sync module can send the same timeFrames[]/paths[] detail
//   Android sends in its Trips-update payload, doc 149 §2.3).
//
// PHASE A (docs/TestFunctionalities/esp32s3_board/calculations-impl/
// 153_2026-07-26_phase_a_gps_accuracy_and_reconciliation_implementation.md)
// added, on top of Phase B's TimeFrame/sync structure, the full GPS-
// accuracy pipeline from doc 150 — this is what actually fixes the
// original "fare runs too fast when GPS is bad/absent" bug:
//   - GPS jitter gate (§5): sub-FARE_CALC_MIN_GAP_DISTANCE_M moves are
//     ignored as noise; gaps over FARE_CALC_MAX_PLAUSIBLE_SPEED_MPS's
//     implied velocity are discarded as glitches; road-snapping (D4)
//     still applies for any plausible gap over DIRECTIONS_MIN_DISTANCE_M.
//   - Same-point freeze detection + GPS trust hysteresis (§8): losing
//     trust is immediate (one bad-HDOP or no-fix tick); regaining it
//     needs FARE_CALC_GPS_MIN_ACCURATE_FOR_REACTIVATION consecutive
//     good-HDOP fixes — the same asymmetric "easy to lose, hard to
//     regain" shape as Android's real accuracy-based hysteresis, HDOP
//     substituted for Android's meter-accuracy field (this GNSS reports
//     dilution-of-precision, not a meters figure).
//   - Graceful GPS-inactive billing (§6): NOT unconditionally punitive
//     any more. This hardware has no sensor-fusion speed estimate or
//     activity-recognition "isMoving" signal (no IMU wired into this
//     board's project), so the LAST KNOWN GPS speed stands in, bounded
//     by FARE_CALC_INACTIVE_GRACE_SEC — see fare_calc.c's tick() comment
//     for the full adapted ladder and why each branch is a reasonable
//     stand-in for Android's original signal.
//   - Retroactive reconciliation on frame close (§7): when a GpsInactive
//     TimeFrame ends, its punitive tick-by-tick guess is REPLACED with
//     the real road distance (via directions_client) between where GPS
//     was lost and where it returned, split into distance/time using the
//     same 25km/h exponential-decay probability curve Android uses.
//
// STILL DELIBERATELY OUT OF SCOPE (not this device's job, or a separate
// later decision, not an oversight):
//   - No manually-forced TIME/DISTANCE modes — only HYBRID (normal
//     speed-based billing), GPS_INACTIVE, and PAUSED are used; trip_
//     manager.c only exposes pause/resume, not a forced-mode switch.
//   - No mid-trip Sedan/Maxi tariff switch — tariff is resolved once at
//     trip start and carried unchanged across every frame.
//
// THREADING:
//   fare_calc_tick() must be called periodically from a task that is
//   NOT the LVGL thread — it does floating-point math, a GPS read, and
//   occasionally a blocking HTTPS call to GraphHopper: for a >250m gap
//   (D4) OR when a GpsInactive TimeFrame closes (Phase A's
//   reconciliation, doc 150 §7). trip_manager.c owns the timer that
//   calls it. All fare_calc_get_snapshot()/fare_calc_get_time_frames()
//   reads are safe to call from the LVGL thread — they just copy out
//   current state.
//
// SERIAL COMMANDS: none directly — controlled via "trip start/stop/
// pause/resume" (backend/taximeter/trip_manager.c), which owns the
// fare_calc lifecycle. Use "trip info" to see live totals.
// ================================================================
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "config.h"   // FARE_CALC_MAX_TIME_FRAMES / FARE_CALC_MAX_POINTS_PER_FRAME
#include "reference_data.h"
#include "directions_client.h"

#define FARE_CALC_TICK_MS  2000   // matches Android's TRIP_POINT_INTERVAL_MILLIS

// 7.2 m/s = 25.92 km/h — the exact speed threshold the reference app uses.
#define FARE_CALC_TIME_RATE_STOPPING_SPEED_MPS  7.2

// GPS-inactive seconds bill at this multiple of the normal time rate
// (the punitive-rate rule).
#define FARE_CALC_GPS_INACTIVE_MULTIPLIER  2.4

// GPS fix is trusted only if its reported HDOP is within this many
// "dilution" units — worse than this, WHILE ALREADY ACTIVE, drops trust
// immediately. (Adapted equivalent of Android's meter-based ACCURACY
// field — this GNSS reports HDOP, not meters.)
#define FARE_CALC_GPS_HDOP_THRESHOLD  5.0

// ── Phase A — GPS jitter gate (doc 150 §5/§12) ──
// Moves smaller than this are ignored as receiver noise, not billed.
#define FARE_CALC_MIN_GAP_DISTANCE_M        5.0
// 36 m/s = 130 km/h — an implied velocity at or above this between two
// consecutive fixes is treated as an impossible jump (a GPS glitch),
// discarded rather than billed.
#define FARE_CALC_MAX_PLAUSIBLE_SPEED_MPS   36.0

// ── Phase A — same-point freeze detection (doc 150 §5/§8/§12) ──
// This many consecutive near-identical fixes, while the last known
// speed was still above the stopping threshold, means the receiver is
// frozen (outputting the same fix repeatedly) rather than the vehicle
// genuinely stationary — forces GPS trust to inactive.
#define FARE_CALC_SAME_POINT_THRESHOLD      4
// Two fixes within this many degrees of each other (lat AND lon) count
// as "the same point". ~0.00001° ≈ 1.1m at the equator — a tight
// epsilon rather than exact float equality, since two independently-
// read doubles for the literal same physical fix can still differ in
// their last bit or two. (Android's own isSamePoint() tolerance wasn't
// verified field-for-field from the source in this pass — this is a
// reasoned equivalent, not a literal port.)
#define FARE_CALC_SAME_POINT_EPSILON_DEG    0.00001

// ── Phase A — GPS trust hysteresis (doc 150 §8/§12) ──
// Stricter HDOP bar required to come BACK from inactive than the one
// that keeps it active (FARE_CALC_GPS_HDOP_THRESHOLD above) — same
// asymmetric "easy to lose, hard to regain" shape as Android's real
// accuracy hysteresis (24m to lose / 10m-times-4 to regain).
#define FARE_CALC_GPS_HDOP_REACTIVATE_MAX          2.5
#define FARE_CALC_GPS_MIN_ACCURATE_FOR_REACTIVATION 4

// ── Phase A — graceful GPS-inactive billing (doc 150 §6, adapted) ──
// While GPS is inactive, if the LAST KNOWN speed (before losing GPS)
// was at/above the stopping threshold, keep estimating distance from
// that speed for up to this many seconds — a bounded "probably still
// moving" grace window — before falling back to the punitive rate. This
// stands in for Android's sensor-fusion estimatedSpeed/isMoving signals,
// which this hardware doesn't have (no IMU wired into this board's
// project) — see fare_calc.c's fare_calc_tick() for the full ladder.
#define FARE_CALC_INACTIVE_GRACE_SEC        15

// ── TimeFrame status — same ordinals as Android's TimeFrameStatus enum
//    (doc 150 §0) so the sync payload's "status" field lines up exactly
//    with what the server already understands from the Android app. ──
typedef enum {
    TIME_FRAME_STABLE = 0,
    TIME_FRAME_GPS_INACTIVE,
    TIME_FRAME_HYBRID,
    TIME_FRAME_TIME,
    TIME_FRAME_DISTANCE,
    TIME_FRAME_PAUSED,
} time_frame_status_t;

// One GPS point stored inside a TimeFrame, for the synced polyline
// (doc 149 §2.3's "paths[]"). Deliberately NOT the full
// TripTimeFrameLocationEntity (speed/accuracy/satelliteCount/battery) —
// this pass stores what's needed to reconstruct the route; extra
// per-point telemetry fields can be added later without breaking the
// sync payload shape (they're optional fields server-side).
typedef struct {
    double lat;
    double lon;
    time_t at;
} time_frame_point_t;

// One TimeFrame segment — the direct analog of Android's
// TripTimeFrameEntity + TripTimeFrameDto combined (doc 150 §0/§9).
typedef struct {
    time_frame_status_t status;
    time_t   start_time;
    time_t   end_time;          // 0 while still open (only meaningful on a CLOSED frame)
    int64_t  tariff_id;

    double   distance_m;             // this frame's OWN billable distance (not the whole-trip odometer)
    double   gps_active_time_s;
    double   gps_inactive_time_s;

    double   start_odometer_m;       // whole-trip odometer AT this frame's start (carried across frames)
    bool     has_threshold;          // mirrors Android's nullable distanceInMeterAtThreshold
    double   distance_at_threshold_m;// only meaningful if has_threshold — see doc 150 §9.2/§3.1

    // Fare, computed once at close (or live, for the still-open frame
    // returned by fare_calc_get_time_frames()) — see fare_calc.c's
    // _finalize_frame_fare().
    double   distance_fare_cents;
    double   time_fare_cents;         // active + inactive combined, this frame only
    double   total_fare_cents;

    time_frame_point_t points[FARE_CALC_MAX_POINTS_PER_FRAME];
    int      point_count;
    bool     points_truncated;      // true if this frame dropped points past the cap — logged once
} time_frame_t;

typedef struct {
    bool    is_running;
    bool    is_paused;
    bool    gps_active;

    double  distance_km;            // whole-trip odometer
    double  speed_kmh;               // instantaneous, from the latest GPS fix

    double  flag_fall_cents;
    double  distance_fare_cents;     // summed across every frame (closed + current)
    double  time_fare_cents;         // GPS-active time billing, summed across every frame
    double  gps_inactive_fare_cents; // GPS-inactive (punitive-rate) time billing, summed
    double  extras_cents;
    double  special_fares_cents;
    double  total_fare_cents;

    double  total_active_time_s;     // raw seconds, summed across every frame — for sync's "duration" field
    double  total_inactive_time_s;   // raw seconds, summed across every frame

    int64_t tariff_id;
    char    tariff_type[16];
    time_t  started_at;
} fare_calc_snapshot_t;

// Begin a new trip's fare calculation against the given tariff. Resets
// every running total to zero (except flag-fall, applied once here),
// and opens the first TimeFrame (Hybrid — matches Android's default
// state at trip start, doc 150 §3).
void fare_calc_start(const tariff_t *tariff);

// Stop — closes the current TimeFrame (if any) and freezes every
// total (still readable via fare_calc_get_snapshot()/
// fare_calc_get_time_frames() afterward; call fare_calc_start() again
// to begin a fresh trip).
void fare_calc_stop(void);

// Pause/resume close/reopen a TimeFrame immediately (a PAUSED segment
// while paused) rather than waiting for the next tick to notice —
// deliberately simpler than Android's continuous-tick-during-pause
// model (doc 150 §3), since trip_manager.c's fare_calc_tick() is a
// no-op entirely while paused (see the .c file).
void fare_calc_pause(void);
void fare_calc_resume(void);

// Call every FARE_CALC_TICK_MS from trip_manager's timer/task while a
// trip is running. No-op if not running or paused.
void fare_calc_tick(void);

// Add a flat amount (extras or a matched special fare) — cents, added
// directly to the respective running total (trip-level, not per-frame —
// matches Android's flag-fall/extras being trip-level additions, doc
// 150 §9.4).
void fare_calc_add_extras(double cents);
void fare_calc_add_special_fare_cents(double cents);

bool fare_calc_is_running(void);

// Copies the current AGGREGATE state out (summed across every closed
// frame + the current one) — safe to call from any task. Field
// meanings are unchanged from the pre-TimeFrame version of this module,
// so existing callers (trip_manager.c's "trip info"/persistence) keep
// working without modification.
void fare_calc_get_snapshot(fare_calc_snapshot_t *out);

// Copies up to max_count TimeFrames into out — every CLOSED frame, plus
// (if a trip is running) the still-open current frame as its last
// entry, reported live (its fare/end_time computed as of "now" WITHOUT
// actually closing it — the real current frame keeps running). This is
// what trip_sync.c reads to build the "timeFrames[]"/"paths[]" arrays
// in the Trips-update payload (doc 149 §2.3). Returns the number of
// frames written.
int fare_calc_get_time_frames(time_frame_t *out, int max_count);

// The tariff this trip is running against — read-only, valid only
// while fare_calc_is_running() (or immediately after fare_calc_stop(),
// before the next fare_calc_start()). Used by trip_sync.c to fill the
// Trips-update payload's tariff-snapshot fields (name/flagFall/rates,
// doc 149 §2.3's InputTripDto.Trip).
const tariff_t *fare_calc_get_active_tariff(void);

// The trip's pickup location — the very first GPS point recorded (from
// the first TimeFrame, closed or still open). Returns false if no
// location has been recorded yet (trip started with no GPS fix at all
// and none has arrived since). Used by trip_sync.c's AddJob payload.
bool fare_calc_get_pickup_location(double *out_lat, double *out_lon);
