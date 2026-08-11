/**
 * fare_calc.c — Live fare calculation + per-segment TimeFrame history
 *
 * See fare_calc.h for the full design and the deliberate simplifications
 * vs. the Android reference (doc 151 §7.1 D2-D5). This is the actual
 * meter: every tick it reads the latest GPS fix, opens/closes TimeFrame
 * segments exactly like the Android reference does, and bills using the
 * same 7.2 m/s threshold + tiered distance rate + road-snapped distance
 * (via directions_client.c) for any gap over DIRECTIONS_MIN_DISTANCE_M.
 */
#include <string.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "gps/gps_client.h"
#include "directions_client.h"
#include "bg_worker.h"
#include "fare_calc.h"

static const char *TAG = "farecalc";

typedef struct {
    bool     running;
    bool     paused;
    tariff_t tariff;
    time_t   started_at;

    double   total_distance_m;   // whole-trip odometer — drives tiering, carried across every frame

    double   extras_cents;         // trip-level (not per-frame) — matches doc 150 §9.4
    double   special_fares_cents;

    bool     has_last_fix;
    double   last_lat, last_lon;
    time_t   last_fix_time;
    double   last_speed_kmh;
    bool     gps_active;          // current tick's determination — exposed in the snapshot
    bool     has_ever_had_fix;    // D2 (doc 151 §7.1) — before this is true, bill plain time, not punitive

    // ── Phase A — GPS trust hysteresis + jitter state (doc 150 §5/§8) ──
    int      same_point_counter;   // consecutive near-identical fixes while active
    int      accurate_fix_streak;  // consecutive good-HDOP fixes while inactive (reactivation progress)
    time_t   gps_lost_at;          // 0 = currently active / not tracked; set the tick trust is lost

    // doc 184 §7.2 — carried-forward baseline from a trip that was
    // running before a reboot (fare_calc_restore()). Added on top of the
    // live TimeFrame totals in fare_calc_get_snapshot() every time it's
    // called — NOT applied to s.tariff.flag_fall_cents (that field is
    // read fresh from s.tariff each snapshot, so it's naturally correct
    // without a separate carried field), and NOT to extras_cents/
    // special_fares_cents (those are already plain trip-level
    // accumulators — fare_calc_restore() seeds them directly instead).
    double   carried_distance_fare_cents;
    double   carried_time_fare_cents;
    double   carried_gps_inactive_fare_cents;

    // ── TimeFrame history ──
    time_frame_t frames[FARE_CALC_MAX_TIME_FRAMES];
    int          frame_count;      // CLOSED frames only
    time_frame_t current;
    bool         has_current;
    bool         frames_truncated; // logged once
} fare_calc_state_t;

// PHASE 0 (doc 179 §2/§6): this struct is ~35KB dominated by
// frames[FARE_CALC_MAX_TIME_FRAMES] (32 x ~1KB, each carrying its own
// points[FARE_CALC_MAX_POINTS_PER_FRAME]). Left as a plain static, that
// entire block sits in internal SRAM .bss permanently — on a board
// measured with 7.4KB of internal heap free (doc 179 §2.1), that is the
// single largest easy win available. It touches no DMA path and is
// never read from an ISR, so PSRAM is safe here. Allocated once by
// fare_calc_init() (called from trip_manager_init(), before any other
// fare_calc_* call) and never freed.
//
// The `s` macro keeps every existing `s.field` access below working
// unchanged — `s.field` expands to `(*s_state).field`, identical to
// `s_state->field` — so this is a pure allocation-strategy change, not
// a rewrite of the module's logic.
static fare_calc_state_t *s_state = NULL;
#define s (*s_state)

// doc 184 §1/§10.3 — the semaphore _directions_get_route_via_bgworker()
// (below) uses to wait for a GraphHopper call that now actually runs on
// bg_worker's task/stack, not trip_tick's. Created once here, alongside
// the PSRAM state allocation.
static SemaphoreHandle_t s_directions_done_sem = NULL;

void fare_calc_init(void) {
    if (s_state) return;   // already initialized — safe to call more than once
    s_state = (fare_calc_state_t *)heap_caps_calloc(1, sizeof(fare_calc_state_t), MALLOC_CAP_SPIRAM);
    if (!s_state) {
        ESP_LOGE(TAG, "fare_calc_init: PSRAM allocation of %u bytes FAILED — meter cannot run",
                 (unsigned)sizeof(fare_calc_state_t));
        return;
    }
    s_directions_done_sem = xSemaphoreCreateBinary();
    if (!s_directions_done_sem) {
        ESP_LOGW(TAG, "fare_calc_init: directions-job semaphore creation failed — road-snapping/reconciliation will always fall back to straight-line");
    }
    ESP_LOGI(TAG, "fare_calc_init: state (%u bytes) allocated in PSRAM, internal SRAM untouched",
             (unsigned)sizeof(fare_calc_state_t));
}

// ── Haversine great-circle distance, meters ──
static double _haversine_m(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double phi1 = lat1 * M_PI / 180.0, phi2 = lat2 * M_PI / 180.0;
    double dphi = (lat2 - lat1) * M_PI / 180.0;
    double dlambda = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dphi / 2) * sin(dphi / 2) + cos(phi1) * cos(phi2) * sin(dlambda / 2) * sin(dlambda / 2);
    return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}

// Tight-epsilon "is this the same point" check — see
// FARE_CALC_SAME_POINT_EPSILON_DEG's comment in fare_calc.h for why an
// epsilon instead of exact equality.
static bool _is_same_point(double lat1, double lon1, double lat2, double lon2) {
    return fabs(lat1 - lat2) < FARE_CALC_SAME_POINT_EPSILON_DEG
        && fabs(lon1 - lon2) < FARE_CALC_SAME_POINT_EPSILON_DEG;
}

// ═══════════════════════════════════════════════════════════════
//  doc 184 §1/§10.3 — GraphHopper calls run on bg_worker, not trip_tick
//
//  Both fare_calc.c call sites (_process_new_fix()'s >250m road-snap,
//  _reconcile_gps_inactive_frame()'s reconciliation) used to call
//  directions_client_get_route() DIRECTLY — a blocking HTTPS/TLS call,
//  on trip_tick's 4096-byte stack. mbedTLS's handshake needs roughly
//  8KB of call-stack depth; this crashed the board 3 times (stack
//  overflow -> reboot -> the ENTIRE active trip lost) the first time
//  GRAPHHOPPER_API_KEY was ever set to a real value (doc 182 D7) and
//  this code path finally got to execute — see doc 184 §1 for the full
//  analysis, and §10 for why "just raise the stack" was rejected (the
//  vendor's own OTA/HTTPS examples all use exactly 8192 bytes for a
//  task that does ONLY the TLS call — trip_tick has real frames above
//  it too, so 8192 would still be short by the same shape of margin
//  that cost 2 days to find in the doc 73 OTA postmortem).
//
//  This wrapper submits the SAME directions_client_get_route() call as
//  a bg_worker job — it runs on bg_worker's 8192-byte stack (already
//  proven safe for TLS by login/reference-data/trip-sync) — and
//  trip_tick BLOCKS on a semaphore waiting for it, rather than doing
//  the network I/O itself. This is a deliberate choice over a fire-
//  and-forget/mailbox design: fare_calc's internal state (frames[],
//  current, total_distance_m) has NO lock and is only ever safe because
//  trip_tick is its ONE writer — a second task (bg_worker) writing
//  results back into that state directly would introduce a real data
//  race. Blocking keeps trip_tick as the sole writer; only the deep
//  TLS call itself moves to a task whose stack was actually sized for
//  it. Billing accuracy/timing is unchanged from before this fix —
//  only WHERE the network call's stack frames live changed.
//
//  The semaphore wait is UNBOUNDED (portMAX_DELAY), not timed, and
//  that's deliberate, not an oversight: `job` is a STACK-LOCAL struct
//  in the calling function below. If we gave up waiting early (a timed
//  wait) and returned, that stack frame could be reused by the next
//  function call — and the bg_worker job, still in flight, would later
//  write its result into what is now unrelated stack memory. Waiting
//  unconditionally avoids that use-after-scope class of bug entirely.
//  This is still bounded in practice: directions_client_get_route()'s
//  own HTTP client timeout (DIRECTIONS_HTTP_TIMEOUT_MS, config.h) caps
//  the job itself, and bg_worker's _worker_task always calls the done
//  callback exactly once after every job — queueing behind other
//  bg_worker jobs can delay this, but every job type in this codebase
//  has its own timeout, so the wait is long-tail-bounded, not infinite.
//  A worst-case delay here means a stale display for a few extra
//  seconds on trip_tick's own task — not the LVGL thread, so the UI
//  stays responsive regardless — which is a far better failure mode
//  than the crash this replaces.
// ═══════════════════════════════════════════════════════════════
typedef struct {
    double from_lat, from_lon, to_lat, to_lon;
    double out_distance_m;
    directions_point_t out_pts[FARE_CALC_MAX_POINTS_PER_FRAME];
    int    out_pt_count;
    bool   out_success;
} _directions_job_arg_t;

static bool _directions_job_fn(void *arg) {
    _directions_job_arg_t *a = (_directions_job_arg_t *)arg;
    a->out_success = directions_client_get_route(a->from_lat, a->from_lon, a->to_lat, a->to_lon,
                                                   &a->out_distance_m, a->out_pts,
                                                   FARE_CALC_MAX_POINTS_PER_FRAME, &a->out_pt_count);
    return a->out_success;
}

static void _directions_job_done(bool success, void *arg, void *user_data) {
    (void)success; (void)arg; (void)user_data;
    xSemaphoreGive(s_directions_done_sem);
}

static bool _directions_get_route_via_bgworker(double from_lat, double from_lon, double to_lat, double to_lon,
                                                 double *out_distance_m, directions_point_t *out_pts,
                                                 int max_pts, int *out_pt_count) {
    if (!s_directions_done_sem) return false;   // creation failed at init — caller falls back to straight-line

    _directions_job_arg_t job = {
        .from_lat = from_lat, .from_lon = from_lon, .to_lat = to_lat, .to_lon = to_lon,
    };
    if (!bg_worker_submit_fn(_directions_job_fn, &job, _directions_job_done, NULL)) {
        ESP_LOGW(TAG, "directions: bg_worker busy — falling back to straight-line this tick");
        return false;
    }

    xSemaphoreTake(s_directions_done_sem, portMAX_DELAY);   // see the file-header comment above for why unbounded

    if (!job.out_success) return false;

    *out_distance_m = job.out_distance_m;
    int n = job.out_pt_count;
    if (n > max_pts) n = max_pts;
    if (n > 0) memcpy(out_pts, job.out_pts, (size_t)n * sizeof(directions_point_t));
    *out_pt_count = n;
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  PHASE A — GPS TRUST HYSTERESIS (doc 150 §8)
//  Losing trust is immediate; regaining it needs several consecutive
//  good-HDOP fixes — the same asymmetric shape as Android's real
//  accuracy-based hysteresis. See fare_calc.h for the HDOP-substitutes-
//  for-meter-accuracy adaptation note.
// ═══════════════════════════════════════════════════════════════
static void _set_gps_active(bool active, const char *reason) {
    if (s.gps_active == active) return;
    s.gps_active = active;
    s.same_point_counter   = 0;
    s.accurate_fix_streak  = 0;
    s.gps_lost_at = active ? 0 : time(NULL);
    ESP_LOGI(TAG, "GPS trust: %s (%s)", active ? "ACTIVE" : "INACTIVE", reason);
}

static void _update_gps_trust(const gps_data_t *g) {
    bool has_fix = (g != NULL) && g->has_fix && g->fix_quality > 0 && g->hdop > 0.0;

    if (s.gps_active) {
        if (!has_fix) {
            _set_gps_active(false, "no fix");
        } else if (g->hdop > FARE_CALC_GPS_HDOP_THRESHOLD) {
            _set_gps_active(false, "HDOP too high");
        }
        // else: stays active, no per-fix streak bookkeeping needed while already trusted
    } else {
        if (has_fix && g->hdop <= FARE_CALC_GPS_HDOP_REACTIVATE_MAX) {
            s.accurate_fix_streak++;
            if (s.accurate_fix_streak >= FARE_CALC_GPS_MIN_ACCURATE_FOR_REACTIVATION) {
                _set_gps_active(true, "reactivation streak complete");
            }
        } else {
            s.accurate_fix_streak = 0;
        }
    }
}

// 24 km/h => 100%, 25 km/h ≈ 87.4%, 30km/h ≈ 43.6%, 36km/h ≈ 18.9% — the
// exact same exponential-decay curve Android uses to split a GPS-
// inactive gap's duration into "moving" (distance-billed) vs "stopped"
// (time-billed) once the real road distance is known (doc 150 §7).
static double _probability_for_25kmh(double avg_speed_mps) {
    const double base_speed = 6.67; // 24 km/h
    if (avg_speed_mps <= base_speed) return 100.0;
    double probability = 100.0 * exp(-0.5 * (avg_speed_mps - base_speed));
    return probability < 0.0 ? 0.0 : probability;
}

// ═══════════════════════════════════════════════════════════════
//  TIMEFRAME HELPERS
// ═══════════════════════════════════════════════════════════════
static double _frame_distance_fare_cents(const time_frame_t *f) {
    double rate1 = s.tariff.distance_rate_cents_per_km / 1000.0;
    double rate2 = s.tariff.distance_rate2_cents_per_km / 1000.0;

    if (!f->has_threshold) {
        return f->distance_m * rate1;
    } else if (f->distance_at_threshold_m <= 0.0) {
        return f->distance_m * rate2;
    } else if (f->distance_m > f->distance_at_threshold_m) {
        return f->distance_at_threshold_m * rate1 + (f->distance_m - f->distance_at_threshold_m) * rate2;
    } else {
        return f->distance_m * rate1;
    }
}

static double _frame_active_time_fare_cents(const time_frame_t *f) {
    return f->gps_active_time_s * (s.tariff.time_rate_cents_per_min / 60.0);
}

static double _frame_inactive_time_fare_cents(const time_frame_t *f) {
    return f->gps_inactive_time_s * (s.tariff.time_rate_cents_per_min / 60.0) * FARE_CALC_GPS_INACTIVE_MULTIPLIER;
}

static void _finalize_frame_fare(time_frame_t *f) {
    f->distance_fare_cents = _frame_distance_fare_cents(f);
    f->time_fare_cents     = _frame_active_time_fare_cents(f) + _frame_inactive_time_fare_cents(f);
    f->total_fare_cents    = f->distance_fare_cents + f->time_fare_cents;
}

// ═══════════════════════════════════════════════════════════════
//  PHASE A — RETROACTIVE GPS-INACTIVE RECONCILIATION (doc 150 §7)
//
//  When a GpsInactive frame closes, its tick-by-tick punitive-rate guess
//  is REPLACED (not adjusted) with the real answer: the actual road
//  distance between where GPS was lost and where it returned, and that
//  gap's duration split into distance-billed/time-billed portions via
//  the 25km/h probability curve. This matches Android's processInactive()
//  exactly — it zeroes gpsInactiveTimeInSec entirely on reconciliation
//  and replaces distanceInMeter/gpsActiveTimeInSec outright, it does NOT
//  do a partial fare deduction (the "remaining fare" variables visible
//  in the Android source are computed and logged but never fed back
//  into the frame's actual stored values — verified by reading
//  updateBackgroundFare()'s real argument list, doc 150 §7).
// ═══════════════════════════════════════════════════════════════
static void _reconcile_gps_inactive_frame(time_frame_t *f, double end_lat, double end_lon) {
    if (f->point_count == 0) return;   // nothing recorded to reconcile against — leave the tick-by-tick guess as-is

    double start_lat = f->points[0].lat;
    double start_lon = f->points[0].lon;
    double old_estimated_distance_m = f->distance_m;   // the grace-window guess accrued tick-by-tick — see the odometer correction below

    // Append the reactivating/closing point to the frame's own points[]
    // — mirrors Android's addLocations(timeFrame.locations) right before
    // processInactive() runs (doc 150 §3.1/§7).
    if (f->point_count < FARE_CALC_MAX_POINTS_PER_FRAME) {
        time_frame_point_t *p = &f->points[f->point_count++];
        p->lat = end_lat;
        p->lon = end_lon;
        p->at  = f->end_time;
    }

    double gap_duration_s = (double)(f->end_time - f->start_time);
    if (gap_duration_s <= 0.0) return;

    double distance_m = 0.0;
    directions_point_t route_pts[FARE_CALC_MAX_POINTS_PER_FRAME];
    int route_pt_count = 0;
    bool used_route = false;
    if (_directions_get_route_via_bgworker(start_lat, start_lon, end_lat, end_lon, &distance_m,
                                            route_pts, FARE_CALC_MAX_POINTS_PER_FRAME, &route_pt_count)) {
        double implied_speed = distance_m / gap_duration_s;
        if (implied_speed <= 0.0 || implied_speed >= FARE_CALC_MAX_PLAUSIBLE_SPEED_MPS) {
            distance_m = _haversine_m(start_lat, start_lon, end_lat, end_lon);   // road result looked implausible — fall back
        } else {
            used_route = true;
        }
    } else {
        distance_m = _haversine_m(start_lat, start_lon, end_lat, end_lon);
    }

    // Fill in the actual road-snapped path between where GPS was lost
    // and where it returned — the frame otherwise only has [start, end]
    // (a straight jump), which would be a poor polyline for a real gap
    // that may span a curving road. Purely a sync-detail enrichment —
    // does not affect the fare_m/active_time_s reconciliation above.
    if (used_route) {
        for (int i = 0; i < route_pt_count && f->point_count < FARE_CALC_MAX_POINTS_PER_FRAME; i++) {
            time_frame_point_t *p = &f->points[f->point_count++];
            p->lat = route_pts[i].lat;
            p->lon = route_pts[i].lon;
            p->at  = f->end_time;   // exact per-point timestamps aren't known for a reconciled gap — end time is the best available
        }
    }

    double average_speed_mps = distance_m / gap_duration_s;
    double probability = _probability_for_25kmh(average_speed_mps);
    double active_time_s = gap_duration_s * (probability / 100.0);

    ESP_LOGI(TAG, "TimeFrame reconciled: gap=%.0fs road_dist=%.1fm avgSpeed=%.1fkm/h -> active=%.0fs "
             "(was: dist=%.1fm activeT=%.0fs inactiveT=%.0fs)",
             gap_duration_s, distance_m, average_speed_mps * 3.6, active_time_s,
             f->distance_m, f->gps_active_time_s, f->gps_inactive_time_s);

    f->distance_m          = distance_m;
    f->gps_active_time_s    = active_time_s;
    f->gps_inactive_time_s  = 0.0;   // fully replaced — no residual punitive time, matches Android exactly (see header comment above)

    // Odometer correction: the grace-window ticks (fare_calc_tick()'s
    // GPS-inactive branch) already added their own estimated distance
    // into BOTH this frame's distance_m AND the whole-trip odometer
    // (s.total_distance_m) as they happened. Now that the real distance
    // is known, correct the odometer by the delta so any tier-threshold
    // crossing this frame caused reflects the truth, not the guess.
    s.total_distance_m += (distance_m - old_estimated_distance_m);
    if (s.total_distance_m < 0.0) s.total_distance_m = 0.0;   // guard against a pathological negative correction
}

// Opens a new current TimeFrame — carries the whole-trip odometer and
// tier-threshold state across the boundary exactly like Android's
// addNewTimeFrame() does (doc 150 §3.1 points 4/5).
static void _open_frame(time_frame_status_t status, double lat, double lon) {
    memset(&s.current, 0, sizeof(s.current));
    s.current.status          = status;
    s.current.start_time      = time(NULL);
    s.current.tariff_id       = s.tariff.tariff_id;
    s.current.start_odometer_m = s.total_distance_m;

    double threshold_m = s.tariff.distance_rate_range_km * 1000.0;
    if (threshold_m > 0.0 && s.total_distance_m >= threshold_m) {
        // The trip already crossed the tier boundary in an earlier
        // frame — this whole new frame bills entirely at tier-2.
        s.current.has_threshold        = true;
        s.current.distance_at_threshold_m = 0.0;
    }

    if (lat != 0.0 || lon != 0.0) {
        s.current.points[0].lat = lat;
        s.current.points[0].lon = lon;
        s.current.points[0].at  = time(NULL);
        s.current.point_count   = 1;
    }

    s.has_current = true;
    ESP_LOGI(TAG, "TimeFrame opened: status=%d (odometer carry-in=%.1fm)", (int)status, s.total_distance_m);
}

static void _close_frame(time_t end_time, double end_lat, double end_lon) {
    if (!s.has_current) return;

    s.current.end_time = end_time;
    if (s.current.status == TIME_FRAME_GPS_INACTIVE) {
        _reconcile_gps_inactive_frame(&s.current, end_lat, end_lon);   // Phase A, doc 150 §7
    }
    _finalize_frame_fare(&s.current);

    if (s.frame_count < FARE_CALC_MAX_TIME_FRAMES) {
        s.frames[s.frame_count++] = s.current;
    } else if (!s.frames_truncated) {
        s.frames_truncated = true;
        ESP_LOGW(TAG, "TimeFrame history full (%d) — this segment's fare still counts toward the trip",
                 FARE_CALC_MAX_TIME_FRAMES);
        ESP_LOGW(TAG, "  total, but its per-segment sync detail will be missing. Raise");
        ESP_LOGW(TAG, "  FARE_CALC_MAX_TIME_FRAMES (config.h) if this happens routinely.");
    }
    ESP_LOGI(TAG, "TimeFrame closed: status=%d distance=%.1fm activeT=%.0fs inactiveT=%.0fs fare=%.2f\xC2\xA2",
             (int)s.current.status, s.current.distance_m, s.current.gps_active_time_s,
             s.current.gps_inactive_time_s, s.current.total_fare_cents);
    s.has_current = false;
}

static void _add_points_to_current_frame(const directions_point_t *pts, int count) {
    if (!s.has_current) return;
    for (int i = 0; i < count; i++) {
        if (s.current.point_count >= FARE_CALC_MAX_POINTS_PER_FRAME) {
            if (!s.current.points_truncated) {
                s.current.points_truncated = true;
                ESP_LOGW(TAG, "TimeFrame's point buffer full (%d) — extra GPS points this segment dropped "
                         "(fare is unaffected; only the synced polyline loses detail)", FARE_CALC_MAX_POINTS_PER_FRAME);
            }
            return;
        }
        time_frame_point_t *p = &s.current.points[s.current.point_count++];
        p->lat = pts[i].lat;
        p->lon = pts[i].lon;
        p->at  = time(NULL);
    }
}

static void _maybe_set_frame_threshold(void) {
    if (!s.has_current || s.current.has_threshold) return;
    double threshold_m = s.tariff.distance_rate_range_km * 1000.0;
    if (threshold_m <= 0.0) return;
    if (s.total_distance_m >= threshold_m) {
        s.current.has_threshold        = true;
        s.current.distance_at_threshold_m = s.current.distance_m;   // split point = frame's own distance so far
    }
}

static void _add_distance_to_current_frame(double distance_m) {
    if (!s.has_current || distance_m <= 0.0) return;
    s.current.distance_m += distance_m;
    _maybe_set_frame_threshold();
}

static void _add_active_time_to_current_frame(double seconds) {
    if (!s.has_current || seconds <= 0.0) return;
    s.current.gps_active_time_s += seconds;
}

static void _add_inactive_time_to_current_frame(double seconds) {
    if (!s.has_current || seconds <= 0.0) return;
    s.current.gps_inactive_time_s += seconds;
}

// ═══════════════════════════════════════════════════════════════
//  MOVEMENT — measure + bill one Hybrid tick's distance/time
//  Phase A (doc 150 §5): same-point freeze detection, sub-5m jitter
//  rejection, and the 130km/h implausible-jump ceiling all run here,
//  BEFORE the D4 road-snap decision (doc 151 §7.1) for any gap over
//  DIRECTIONS_MIN_DISTANCE_M.
// ═══════════════════════════════════════════════════════════════
static void _process_new_fix(const gps_data_t *g, double elapsed_sec) {
    // ── Same-point freeze detection (doc 150 §5/§8) ──
    if (s.has_last_fix) {
        if (_is_same_point(s.last_lat, s.last_lon, g->lat, g->lon)) {
            s.same_point_counter++;
            if (s.same_point_counter >= FARE_CALC_SAME_POINT_THRESHOLD
                && (s.last_speed_kmh / 3.6) >= FARE_CALC_TIME_RATE_STOPPING_SPEED_MPS) {
                // Receiver is outputting the same fix repeatedly while
                // the vehicle was apparently still moving — that's a
                // frozen GPS, not a genuinely stationary taxi. Force
                // trust to inactive; this tick still bills below (the
                // Hybrid frame's own "no movement -> bill time" branch
                // naturally covers it — no separate early return needed).
                _set_gps_active(false, "same point while apparently moving");
            }
        } else {
            s.same_point_counter = 0;
        }
    }

    // ── Measure movement (doc 150 §5) ──
    double moved_m = 0.0;
    bool   used_route = false;
    directions_point_t route_pts[FARE_CALC_MAX_POINTS_PER_FRAME];
    int    route_pt_count = 0;

    if (s.has_last_fix) {
        double straight_m = _haversine_m(s.last_lat, s.last_lon, g->lat, g->lon);

        if (straight_m < FARE_CALC_MIN_GAP_DISTANCE_M) {
            moved_m = 0.0;   // sub-5m — receiver jitter, not real movement
        } else if (straight_m <= DIRECTIONS_MIN_DISTANCE_M) {
            moved_m = straight_m;
        } else {
            double dt = (double)(time(NULL) - s.last_fix_time);
            double implied_velocity = (dt > 0.0) ? (straight_m / dt) : 0.0;
            if (implied_velocity <= 0.0 || implied_velocity >= FARE_CALC_MAX_PLAUSIBLE_SPEED_MPS) {
                moved_m = 0.0;   // implausible jump (>=130km/h implied) — discard as a glitch
            } else if (_directions_get_route_via_bgworker(s.last_lat, s.last_lon, g->lat, g->lon, &moved_m,
                                                            route_pts, FARE_CALC_MAX_POINTS_PER_FRAME, &route_pt_count)) {
                used_route = true;
            } else {
                moved_m = straight_m;   // directions_client already logged why it fell back
            }
        }
    }

    s.last_lat = g->lat;
    s.last_lon = g->lon;
    s.last_fix_time = time(NULL);
    s.has_last_fix = true;
    s.last_speed_kmh = g->speed;

    if (moved_m > 0.0) {
        s.total_distance_m += moved_m;
    }

    if (s.has_current) {
        if (used_route && route_pt_count > 0) {
            _add_points_to_current_frame(route_pts, route_pt_count);
        } else {
            directions_point_t p = { .lat = g->lat, .lon = g->lon };
            _add_points_to_current_frame(&p, 1);
        }
    }

    double speed_mps = g->speed / 3.6;
    if (moved_m > 0.0 && speed_mps >= FARE_CALC_TIME_RATE_STOPPING_SPEED_MPS) {
        _add_distance_to_current_frame(moved_m);
    } else {
        _add_active_time_to_current_frame(elapsed_sec);
    }
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════
void fare_calc_start(const tariff_t *tariff) {
    if (!s_state) {
        ESP_LOGE(TAG, "start: fare_calc_init() was never called (or its PSRAM allocation failed) — refusing to start");
        return;
    }
    if (!tariff) {
        ESP_LOGE(TAG, "start: NULL tariff — refusing to start with no rates");
        return;
    }
    memset(&s, 0, sizeof(s));
    s.tariff       = *tariff;
    s.running      = true;
    s.paused       = false;
    s.started_at   = time(NULL);
    s.gps_active   = true;   // matches Android's GpsTracker starting isGpsActive=true, doc 150 §8

    double start_lat = 0.0, start_lon = 0.0;
    const gps_data_t *g = gps_client_get_latest();
    if (g && g->has_fix) {
        start_lat = g->lat;
        start_lon = g->lon;
        s.has_last_fix   = true;
        s.last_lat       = g->lat;
        s.last_lon       = g->lon;
        s.last_fix_time  = time(NULL);
        s.has_ever_had_fix = true;
        ESP_LOGI(TAG, "  Pickup location: %.6f, %.6f", start_lat, start_lon);
    } else {
        ESP_LOGW(TAG, "  No GPS fix yet at trip start — billing plain time until the first fix arrives (D2, doc 151 §7.1)");
    }

    _open_frame(TIME_FRAME_HYBRID, start_lat, start_lon);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "METER STARTED — tariff #%lld \"%s\" (%s) flagfall=%.2f\xC2\xA2 distRate=%.2f\xC2\xA2/km "
             "distRate2=%.2f\xC2\xA2/km@%.1fkm timeRate=%.2f\xC2\xA2/min",
             (long long)tariff->tariff_id, tariff->name, tariff->type, tariff->flag_fall_cents,
             tariff->distance_rate_cents_per_km, tariff->distance_rate2_cents_per_km,
             tariff->distance_rate_range_km, tariff->time_rate_cents_per_min);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

void fare_calc_restore(const tariff_t *tariff, const fare_calc_snapshot_t *carried) {
    if (!s_state) {
        ESP_LOGE(TAG, "restore: fare_calc_init() was never called (or its PSRAM allocation failed) — refusing to restore");
        return;
    }
    if (!tariff || !carried) {
        ESP_LOGE(TAG, "restore: NULL tariff or carried snapshot — refusing");
        return;
    }
    memset(&s, 0, sizeof(s));
    s.tariff       = *tariff;
    s.running      = true;
    s.paused       = false;
    s.started_at   = carried->started_at;   // ORIGINAL start time, not "now" — trip duration must survive the restart
    s.gps_active   = true;                  // matches fare_calc_start()'s own default

    // The carried baseline — everything accrued before the reboot.
    // total_distance_m is the whole-trip odometer directly (no separate
    // carried field needed — _open_frame() below reads it for tier-
    // threshold carry-in exactly like any other frame boundary).
    s.total_distance_m               = carried->distance_km * 1000.0;
    s.carried_distance_fare_cents    = carried->distance_fare_cents;
    s.carried_time_fare_cents        = carried->time_fare_cents;
    s.carried_gps_inactive_fare_cents = carried->gps_inactive_fare_cents;
    s.extras_cents                   = carried->extras_cents;
    s.special_fares_cents            = carried->special_fares_cents;

    double start_lat = 0.0, start_lon = 0.0;
    const gps_data_t *g = gps_client_get_latest();
    if (g && g->has_fix) {
        start_lat = g->lat;
        start_lon = g->lon;
        s.has_last_fix     = true;
        s.last_lat         = g->lat;
        s.last_lon         = g->lon;
        s.last_fix_time    = time(NULL);
        s.has_ever_had_fix = true;
    } else {
        ESP_LOGW(TAG, "  No GPS fix yet at restore — billing plain time until the first fix arrives (same D2 rule as a fresh start)");
    }

    _open_frame(TIME_FRAME_HYBRID, start_lat, start_lon);

    double carried_total_cents = carried->flag_fall_cents + carried->distance_fare_cents + carried->time_fare_cents +
                                  carried->gps_inactive_fare_cents + carried->extras_cents + carried->special_fares_cents;
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "METER RESTORED after reboot — tariff #%lld \"%s\" (%s), carried forward %.2f\xC2\xA2 ($%.2f) "
             "and %.3fkm from before the reboot",
             (long long)tariff->tariff_id, tariff->name, tariff->type,
             carried_total_cents, carried_total_cents / 100.0, carried->distance_km);
    ESP_LOGI(TAG, "  (per-segment TimeFrame history before the reboot could not be recovered — only");
    ESP_LOGI(TAG, "  the aggregate totals; the fare total above is exact, sync detail for that portion is not)");
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

void fare_calc_stop(void) {
    if (!s_state || !s.running) return;

    if (s.has_current) _close_frame(time(NULL), s.last_lat, s.last_lon);
    s.running = false;

    fare_calc_snapshot_t snap;
    fare_calc_get_snapshot(&snap);
    ESP_LOGI(TAG, "METER STOPPED — distance=%.2fkm total=%.2f\xC2\xA2 (%d TimeFrame segment(s))",
             snap.distance_km, snap.total_fare_cents, s.frame_count);
}

void fare_calc_pause(void) {
    if (!s_state || !s.running || s.paused) return;
    if (s.has_current) _close_frame(time(NULL), s.last_lat, s.last_lon);
    _open_frame(TIME_FRAME_PAUSED, s.last_lat, s.last_lon);
    s.paused = true;
    ESP_LOGI(TAG, "METER PAUSED");
}

void fare_calc_resume(void) {
    if (!s_state || !s.running || !s.paused) return;
    if (s.has_current) _close_frame(time(NULL), s.last_lat, s.last_lon);
    time_frame_status_t next = s.gps_active ? TIME_FRAME_HYBRID : TIME_FRAME_GPS_INACTIVE;
    _open_frame(next, s.last_lat, s.last_lon);
    s.paused = false;
    ESP_LOGI(TAG, "METER RESUMED");
}

void fare_calc_tick(void) {
    if (!s_state || !s.running || s.paused) return;

    const double elapsed_sec = FARE_CALC_TICK_MS / 1000.0;
    const gps_data_t *g = gps_client_get_latest();

    // D2 (doc 151 §7.1): before the very first-ever fix of this trip,
    // bill plain active time on whatever frame is open — not nothing,
    // and not the GPS-inactive punitive rate (see fare_calc.h's header
    // comment for why: a taxi waiting at pickup for a fix to arrive is
    // legitimately "stationary, billing time", same as Android's own
    // Time-status billing elsewhere in the meter).
    if (!s.has_ever_had_fix) {
        if (g != NULL && g->has_fix) {
            s.has_ever_had_fix = true;
            // fall through — process this first real fix normally below
        } else {
            _add_active_time_to_current_frame(elapsed_sec);
            return;
        }
    }

    // Phase A: stateful trust hysteresis (doc 150 §8) — replaces the
    // flat per-tick HDOP check. Losing trust is immediate; regaining it
    // needs FARE_CALC_GPS_MIN_ACCURATE_FOR_REACTIVATION consecutive
    // good-HDOP fixes (_update_gps_trust mutates s.gps_active/
    // s.accurate_fix_streak/s.gps_lost_at directly).
    bool gps_active_before = s.gps_active;
    _update_gps_trust(g);
    bool gps_active  = s.gps_active;
    bool gps_flipped = (gps_active != gps_active_before);

    time_frame_status_t desired;
    if (gps_flipped) {
        desired = gps_active ? TIME_FRAME_HYBRID : TIME_FRAME_GPS_INACTIVE;
    } else if (!gps_active) {
        desired = TIME_FRAME_GPS_INACTIVE;
    } else {
        desired = TIME_FRAME_HYBRID;
    }

    if (!s.has_current || s.current.status != desired) {
        double new_frame_lat = s.has_last_fix ? s.last_lat : (g ? g->lat : 0.0);
        double new_frame_lon = s.has_last_fix ? s.last_lon : (g ? g->lon : 0.0);
        // The frame being CLOSED gets the freshest position we have as
        // its end point: the just-arrived fix if GPS is active THIS
        // tick (the reactivation case, feeding Phase A's reconciliation
        // in _close_frame), otherwise the last known position.
        double close_end_lat = (gps_active && g != NULL) ? g->lat : new_frame_lat;
        double close_end_lon = (gps_active && g != NULL) ? g->lon : new_frame_lon;
        if (s.has_current) _close_frame(time(NULL), close_end_lat, close_end_lon);
        _open_frame(desired, new_frame_lat, new_frame_lon);
    }

    if (gps_active && g != NULL) {
        _process_new_fix(g, elapsed_sec);
    } else {
        // Phase A graceful GPS-inactive ladder (doc 150 §6, adapted —
        // see fare_calc.h's FARE_CALC_INACTIVE_GRACE_SEC comment for
        // why the last known GPS speed stands in for Android's sensor-
        // fusion estimatedSpeed/isMoving signals, which this hardware
        // doesn't have):
        double last_speed_mps = s.last_speed_kmh / 3.6;
        time_t inactive_elapsed = s.gps_lost_at > 0 ? (time(NULL) - s.gps_lost_at) : 0;

        if (last_speed_mps >= FARE_CALC_TIME_RATE_STOPPING_SPEED_MPS
            && inactive_elapsed <= FARE_CALC_INACTIVE_GRACE_SEC) {
            // Was clearly moving right before GPS was lost, and we're
            // still inside the short grace window — keep estimating
            // distance from that last known speed rather than assuming
            // stopped or guessing punitively (Android's
            // estimatedSpeed>=7.2 branch).
            double est_distance = last_speed_mps * elapsed_sec;
            s.total_distance_m += est_distance;
            _add_distance_to_current_frame(est_distance);
        } else if (last_speed_mps < FARE_CALC_TIME_RATE_STOPPING_SPEED_MPS) {
            // Was already slow/stopped when GPS was lost — bill plain
            // time, not punitive (Android's isMoving==false branch;
            // "recently near-zero speed" stands in for that signal here).
            _add_active_time_to_current_frame(elapsed_sec);
        } else {
            // Was moving, but the grace window elapsed with no fix
            // back — no longer safe to keep guessing distance; fall to
            // the punitive rate (Android's final "unknown" branch). This
            // frame's punitive guess gets REPLACED (not adjusted) by
            // real data if/when it closes via reconciliation (doc 150 §7).
            _add_inactive_time_to_current_frame(elapsed_sec);
        }
    }
}

void fare_calc_add_extras(double cents)            { if (s_state) s.extras_cents += cents; }
void fare_calc_add_special_fare_cents(double cents) { if (s_state) s.special_fares_cents += cents; }

bool fare_calc_is_running(void) { return s_state && s.running; }

void fare_calc_get_snapshot(fare_calc_snapshot_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_state) return;   // not yet initialized — return the zeroed snapshot

    out->is_running = s.running;
    out->is_paused  = s.paused;
    out->gps_active = s.gps_active;
    out->distance_km = s.total_distance_m / 1000.0;
    out->speed_kmh    = s.last_speed_kmh;
    out->tariff_id    = s.tariff.tariff_id;
    strlcpy(out->tariff_type, s.tariff.type, sizeof(out->tariff_type));
    out->started_at   = s.started_at;
    out->flag_fall_cents = s.tariff.flag_fall_cents;

    double distance_fare = 0.0, active_time_fare = 0.0, inactive_time_fare = 0.0;
    double active_time_s = 0.0, inactive_time_s = 0.0;
    for (int i = 0; i < s.frame_count; i++) {
        distance_fare      += _frame_distance_fare_cents(&s.frames[i]);
        active_time_fare    += _frame_active_time_fare_cents(&s.frames[i]);
        inactive_time_fare  += _frame_inactive_time_fare_cents(&s.frames[i]);
        active_time_s       += s.frames[i].gps_active_time_s;
        inactive_time_s      += s.frames[i].gps_inactive_time_s;
    }
    if (s.has_current) {
        distance_fare      += _frame_distance_fare_cents(&s.current);
        active_time_fare    += _frame_active_time_fare_cents(&s.current);
        inactive_time_fare  += _frame_inactive_time_fare_cents(&s.current);
        active_time_s       += s.current.gps_active_time_s;
        inactive_time_s      += s.current.gps_inactive_time_s;
    }

    // doc 184 §7.2 — carried_* is zero for a normal (non-restored) trip,
    // so this is a no-op in the common case; for a restored trip it adds
    // back whatever had accrued before the reboot (see fare_calc_restore()).
    out->distance_fare_cents     = distance_fare + s.carried_distance_fare_cents;
    out->time_fare_cents          = active_time_fare + s.carried_time_fare_cents;
    out->gps_inactive_fare_cents  = inactive_time_fare + s.carried_gps_inactive_fare_cents;
    out->extras_cents             = s.extras_cents;
    out->special_fares_cents      = s.special_fares_cents;
    out->total_active_time_s      = active_time_s;
    out->total_inactive_time_s    = inactive_time_s;

    out->total_fare_cents = out->flag_fall_cents + out->distance_fare_cents + out->time_fare_cents +
                             out->gps_inactive_fare_cents + out->extras_cents + out->special_fares_cents;
}

int fare_calc_get_time_frames(time_frame_t *out, int max_count) {
    if (!out || max_count <= 0 || !s_state) return 0;

    int n = 0;
    for (int i = 0; i < s.frame_count && n < max_count; i++) {
        out[n++] = s.frames[i];
    }
    if (s.has_current && n < max_count) {
        time_frame_t live = s.current;
        live.end_time = time(NULL);   // reporting-only snapshot — does NOT close the real current frame
        _finalize_frame_fare(&live);
        out[n++] = live;
    }
    return n;
}

const tariff_t *fare_calc_get_active_tariff(void) {
    static const tariff_t s_empty_tariff = {0};
    return s_state ? &s.tariff : &s_empty_tariff;
}

bool fare_calc_get_pickup_location(double *out_lat, double *out_lon) {
    if (!s_state) return false;
    const time_frame_t *first = (s.frame_count > 0) ? &s.frames[0] : (s.has_current ? &s.current : NULL);
    if (!first || first->point_count == 0) return false;
    if (out_lat) *out_lat = first->points[0].lat;
    if (out_lon) *out_lon = first->points[0].lon;
    return true;
}
