#pragma once
// ================================================================
// directions_client.h — Road-snapped distance (GraphHopper)
//
// WHAT THIS MODULE DOES:
//   Port of the Android reference's GetDirectionsUseCase/
//   GraphHopperInstance (doc 149 §4.3) — given two lat/lon points,
//   asks GraphHopper's routing API for the REAL road-following
//   distance between them (and the route's own shape points), instead
//   of the straight-line haversine chord fare_calc.c already computes
//   on its own. A straight line always under-counts distance on any
//   curving road (doc 150 §5/§7), so fare_calc.c only calls this for
//   gaps bigger than DIRECTIONS_MIN_DISTANCE_M (config.h, 250m —
//   matches Android's GH_DIRECTIONS_MIN_DISTANCE exactly) — short gaps
//   stay straight-line, which is accurate enough at that scale.
//
//   Used from TWO call sites in fare_calc.c (doc 150 §5 and §7):
//     1. Live tick billing — a >250m jump between two consecutive
//        accepted fixes gets road-snapped before being billed as
//        distance.
//     2. TimeFrame close — when a GpsInactive segment ends, this is
//        called once to get the real road distance between where GPS
//        was lost and where it came back (the reconciliation Android
//        calls processInactive(), doc 150 §7). Phase A (a later pass,
//        doc 151 §7.1 D5) will add the full probabilistic time/distance
//        split on top of this distance — this module only supplies
//        the distance + shape points, not the fare-split logic.
//
// GATING: DIRECTIONS_ENABLED (config.h) plus a non-empty
// GRAPHHOPPER_API_KEY are both required for a real call — if either is
// off/empty, or if the HTTPS call itself fails for any reason,
// directions_client_get_route() returns false and the caller (fare_calc.c)
// falls back to straight-line haversine automatically. This is a
// "best-effort enhancement," never a hard dependency — the meter must
// keep billing correctly even with no internet reachable to
// GraphHopper specifically (a different failure than "no internet at
// all," e.g. GraphHopper's own service being down).
//
// THREADING: makes a blocking HTTPS call — same rule as api_client.c's
// _perform(): never call this from the LVGL thread. fare_calc_tick()
// itself runs on trip_manager's own tick task (not LVGL), so this is
// safe there, but a >250m gap or an ended GpsInactive frame will add
// one extra blocking HTTPS round-trip to that tick — see the .c file's
// note on why this is acceptable (rare event, not every-tick).
// ================================================================
#include <stdbool.h>
#include <stddef.h>

// One route "shape point" — used both for the road polyline and to
// feed fare_calc.c's per-TimeFrame points[] array (config.h's
// FARE_CALC_MAX_POINTS_PER_FRAME bounds how many of these get kept).
typedef struct {
    double lat;
    double lon;
} directions_point_t;

// Ask GraphHopper for the real road-following distance + shape points
// between (from_lat,from_lon) and (to_lat,to_lon).
//
// out_distance_m       — real road distance, meters (NOT the straight
//                         line) — always written when this returns true.
// out_points           — caller-owned array, filled with the route's
//                         shape points (decoded polyline), oldest-to-
//                         newest; always includes the "to" point as
//                         its last entry so callers don't need to
//                         append it separately.
// out_points_max        — capacity of out_points.
// out_point_count       — how many of out_points were actually filled.
//
// Returns false (leaves *out_distance_m/*out_point_count untouched) if:
//   - DIRECTIONS_ENABLED is 0, or GRAPHHOPPER_API_KEY is empty, or
//   - the HTTPS call fails / times out / returns a non-2xx status, or
//   - the response can't be parsed as a valid GraphHopper route.
// Callers MUST fall back to straight-line haversine on false — this is
// the expected, normal outcome whenever GraphHopper isn't reachable or
// isn't configured, not necessarily an error worth surfacing to the driver.
bool directions_client_get_route(double from_lat, double from_lon, double to_lat, double to_lon,
                                  double *out_distance_m,
                                  directions_point_t *out_points, int out_points_max, int *out_point_count);

// Serial command handler ("directions ...") — "directions test <lat1> <lon1> <lat2> <lon2>"
// for manually exercising a real GraphHopper call from the bench without
// needing a live trip running.
bool directions_client_process_command(const char *line);
