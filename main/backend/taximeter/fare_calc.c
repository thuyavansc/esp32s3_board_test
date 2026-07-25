/**
 * fare_calc.c — Live fare calculation
 *
 * See fare_calc.h for the full design and the deliberate simplifications
 * vs. the Android reference. This is the actual meter: every tick it
 * reads the latest GPS fix and decides distance-rate vs. time-rate
 * billing using the exact same 7.2 m/s threshold the reference app uses.
 */
#include <string.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "gps/gps_client.h"
#include "fare_calc.h"

static const char *TAG = "farecalc";

typedef struct {
    bool    running;
    bool    paused;
    tariff_t tariff;
    time_t   started_at;

    double  total_distance_m;
    double  gps_active_time_s;
    double  gps_inactive_time_s;
    double  extras_cents;
    double  special_fares_cents;

    bool    has_last_fix;
    double  last_lat, last_lon;
    double  last_speed_kmh;
    bool    gps_active;   // current tick's determination — exposed in the snapshot
} fare_calc_state_t;

static fare_calc_state_t s = {0};

// ── Haversine great-circle distance, meters ──
static double _haversine_m(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double phi1 = lat1 * M_PI / 180.0, phi2 = lat2 * M_PI / 180.0;
    double dphi = (lat2 - lat1) * M_PI / 180.0;
    double dlambda = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dphi / 2) * sin(dphi / 2) + cos(phi1) * cos(phi2) * sin(dlambda / 2) * sin(dlambda / 2);
    return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}

void fare_calc_start(const tariff_t *tariff) {
    if (!tariff) {
        ESP_LOGE(TAG, "start: NULL tariff — refusing to start with no rates");
        return;
    }
    memset(&s, 0, sizeof(s));
    s.tariff  = *tariff;
    s.running = true;
    s.paused  = false;
    s.started_at = time(NULL);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "METER STARTED — tariff #%lld \"%s\" (%s) flagfall=%.2f\xC2\xA2 distRate=%.2f\xC2\xA2/km "
             "distRate2=%.2f\xC2\xA2/km@%.1fkm timeRate=%.2f\xC2\xA2/min",
             (long long)tariff->tariff_id, tariff->name, tariff->type, tariff->flag_fall_cents,
             tariff->distance_rate_cents_per_km, tariff->distance_rate2_cents_per_km,
             tariff->distance_rate_range_km, tariff->time_rate_cents_per_min);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

void fare_calc_stop(void) {
    if (!s.running) return;

    fare_calc_snapshot_t snap;
    fare_calc_get_snapshot(&snap);   // totals computed from running accumulators — valid before OR after the flag flip below
    s.running = false;

    ESP_LOGI(TAG, "METER STOPPED — distance=%.2fkm total=%.2f\xC2\xA2", snap.distance_km, snap.total_fare_cents);
}

void fare_calc_pause(void)  { if (s.running) { s.paused = true;  ESP_LOGI(TAG, "METER PAUSED"); } }
void fare_calc_resume(void) { if (s.running) { s.paused = false; ESP_LOGI(TAG, "METER RESUMED"); } }

void fare_calc_tick(void) {
    if (!s.running || s.paused) return;

    const gps_data_t *g = gps_client_get_latest();
    const double elapsed_sec = FARE_CALC_TICK_MS / 1000.0;

    // gps_client reports HDOP (dilution of precision), not a
    // meter-accuracy figure the way Android's Location API does — HDOP
    // <= 5.0 is a commonly-used "good fix" threshold, used here as the
    // closest available equivalent to the accuracy gate. This is an
    // adapted equivalent for this hardware, not a literal port of the
    // same field.
    bool gps_active = (g != NULL) && g->has_fix && g->fix_quality > 0 && g->hdop <= 5.0 && g->hdop > 0.0;
    s.gps_active = gps_active;

    if (gps_active) {
        s.last_speed_kmh = g->speed;

        double moved_m = 0.0;
        if (s.has_last_fix) {
            moved_m = _haversine_m(s.last_lat, s.last_lon, g->lat, g->lon);
        }
        s.last_lat = g->lat;
        s.last_lon = g->lon;
        s.has_last_fix = true;

        double speed_mps = g->speed / 3.6;
        if (moved_m > 0.0 && speed_mps >= FARE_CALC_TIME_RATE_STOPPING_SPEED_MPS) {
            s.total_distance_m += moved_m;
        } else {
            s.gps_active_time_s += elapsed_sec;
        }
    } else {
        // GPS-inactive: bill time at the punitive rate, no distance
        // accrual — no retroactive reconciliation when GPS returns
        // (the simpler recommended default for ESP32).
        s.gps_inactive_time_s += elapsed_sec;
    }
}

void fare_calc_add_extras(double cents)             { s.extras_cents += cents; }
void fare_calc_add_special_fare_cents(double cents)  { s.special_fares_cents += cents; }

bool fare_calc_is_running(void) { return s.running; }

void fare_calc_get_snapshot(fare_calc_snapshot_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->is_running = s.running;
    out->is_paused  = s.paused;
    out->gps_active = s.gps_active;
    out->distance_km = s.total_distance_m / 1000.0;
    out->speed_kmh    = s.last_speed_kmh;
    out->tariff_id    = s.tariff.tariff_id;
    strlcpy(out->tariff_type, s.tariff.type, sizeof(out->tariff_type));
    out->started_at   = s.started_at;

    out->flag_fall_cents = s.tariff.flag_fall_cents;

    // Tiered distance rate — applied against the WHOLE-TRIP running
    // odometer (see fare_calc.h's header comment for why a single
    // running total is equivalent to Android's per-segment sum for this
    // specific purpose).
    double threshold_m = s.tariff.distance_rate_range_km * 1000.0;
    double rate1 = s.tariff.distance_rate_cents_per_km / 1000.0;   // cents per meter
    double rate2 = s.tariff.distance_rate2_cents_per_km / 1000.0;
    if (threshold_m <= 0.0 || s.total_distance_m <= threshold_m) {
        out->distance_fare_cents = s.total_distance_m * rate1;
    } else {
        out->distance_fare_cents = threshold_m * rate1 + (s.total_distance_m - threshold_m) * rate2;
    }

    out->time_fare_cents         = (s.gps_active_time_s / 60.0) * s.tariff.time_rate_cents_per_min;
    out->gps_inactive_fare_cents = (s.gps_inactive_time_s / 60.0) * s.tariff.time_rate_cents_per_min * FARE_CALC_GPS_INACTIVE_MULTIPLIER;
    out->extras_cents            = s.extras_cents;
    out->special_fares_cents     = s.special_fares_cents;

    out->total_fare_cents = out->flag_fall_cents + out->distance_fare_cents + out->time_fare_cents +
                             out->gps_inactive_fare_cents + out->extras_cents + out->special_fares_cents;
}
