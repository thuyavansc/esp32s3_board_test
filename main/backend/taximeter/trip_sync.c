/**
 * trip_sync.c — Trip-to-server sync (AddJob -> Trips -> SaveJobFares)
 *
 * See trip_sync.h for the full design, the D1 identity/token-refresh
 * rationale, and the explicit "not implemented this pass" list. Every
 * JSON body here mirrors the Android reference's own DTOs field-by-
 * field (docs/TestFunctionalities/esp32s3_board/calculations-impl/
 * 149_2026-07-26_android_feature_endpoint_map_and_data_flow.md §2.3) —
 * empty-string/zero placeholders are used ONLY for fields this ESP32
 * genuinely has no data source for yet (reverse-geocoded addresses,
 * structured per-item special fares/tolls), never as a stand-in for a
 * field that should have real data.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "config.h"
#include "api_client.h"
#include "auth_client.h"
#include "session_store.h"
#include "fare_calc.h"
#include "gps/gps_client.h"
#include "bg_worker.h"
#include "rest_api_storage.h"
#include "trip_sync.h"

static const char *TAG = "tripsync";

// Persists across the AddJob/Trips/SaveJobFares calls for one trip —
// small, self-contained, same precedent as auth_client.c's own local
// statics for things nothing else needs to read.
static char s_customer_name[32] = {0};

static bool     s_last_sync_ok   = false;
static time_t   s_last_sync_at   = 0;
static char     s_last_sync_note[64] = "never";

// ── Device MAC address — same format as Android's DeviceDetails.DEVICE_ID
//    (mirrors auth_client.c's own identical helper; not shared via a
//    common utility header since it's a 3-line, single-purpose helper) ──
static void _get_mac_address(char *out, size_t out_size) {
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void _iso8601_utc(time_t t, char *out, size_t out_size) {
    struct tm tm_t;
    gmtime_r(&t, &tm_t);
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_t);
}

// ── D1 follow-on (doc 151 §7.1): retry once, after a fresh re-login,
//    on a 401 — the ESP32 equivalent of Android's AuthInterceptor
//    auto-refresh. Confined here rather than added to api_client.c
//    (see trip_sync.h's header comment for why). ──
static esp_err_t _request_with_reauth(api_method_t method, const char *path, const char *json_body,
                                       char *out_buf, size_t out_buf_size, int *out_status) {
    esp_err_t err = api_client_request(method, path, json_body, true, out_buf, out_buf_size, out_status);
    if (err == ESP_OK && out_status && *out_status == 401) {
        ESP_LOGW(TAG, "  401 on %s — access token likely expired mid-shift, re-logging in once and retrying", path);
        if (auth_client_login(AUTH_TEST_USERNAME, AUTH_TEST_PASSWORD) == ESP_OK) {
            err = api_client_request(method, path, json_body, true, out_buf, out_buf_size, out_status);
        } else {
            ESP_LOGE(TAG, "  Re-login failed — giving up on %s for this attempt", path);
        }
    }
    return err;
}

// ═══════════════════════════════════════════════════════════════
//  1. AddJob — POST taxis-api/api/Job/AddJobByDriver
// ═══════════════════════════════════════════════════════════════
static char *_build_add_job_json(const char *customer_name, double pickup_lat, double pickup_lon) {
    char mac[18];
    _get_mac_address(mac, sizeof(mac));

    char pickup_time[32];
    _iso8601_utc(time(NULL), pickup_time, sizeof(pickup_time));

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "macAddress", mac);
    cJSON_AddStringToObject(body, "fromCity", "");
    cJSON_AddStringToObject(body, "toCity", "");
    cJSON_AddNumberToObject(body, "fromLatitude", pickup_lat);
    cJSON_AddNumberToObject(body, "fromLongitude", pickup_lon);
    cJSON_AddNumberToObject(body, "toLatitude", 0.0);
    cJSON_AddNumberToObject(body, "toLongitude", 0.0);
    cJSON_AddNumberToObject(body, "vehicleId", (double)session_store_get_vehicle_id());
    cJSON_AddNumberToObject(body, "driverId", (double)session_store_get_driver_id());
    cJSON_AddStringToObject(body, "customerFirstName", customer_name ? customer_name : "");
    cJSON_AddStringToObject(body, "streetNo", "");
    cJSON_AddStringToObject(body, "streetName", "");
    cJSON_AddStringToObject(body, "suburb", "");
    cJSON_AddStringToObject(body, "state", "");
    cJSON_AddStringToObject(body, "country", "");
    cJSON_AddStringToObject(body, "postCode", "");
    cJSON_AddStringToObject(body, "note", "");
    cJSON_AddStringToObject(body, "customerMiddleName", "");
    cJSON_AddStringToObject(body, "customerLastName", "");
    cJSON_AddStringToObject(body, "customerDateOfBirth", "");
    cJSON_AddStringToObject(body, "customerTelephone", "");
    cJSON_AddStringToObject(body, "customerEmail", "");
    cJSON_AddStringToObject(body, "pickContactNo", "");
    cJSON_AddStringToObject(body, "pickupTime", pickup_time);
    cJSON_AddNumberToObject(body, "pickLatitute", pickup_lat);    // DTO's own field name — exact spelling matters to the server
    cJSON_AddNumberToObject(body, "pickLongitute", pickup_lon);

    cJSON *pickup_address = cJSON_CreateObject();
    cJSON_AddStringToObject(pickup_address, "address", "");
    cJSON_AddStringToObject(pickup_address, "streetNo", "");
    cJSON_AddStringToObject(pickup_address, "streetName", "");
    cJSON_AddStringToObject(pickup_address, "suburb", "");
    cJSON_AddStringToObject(pickup_address, "state", "");
    cJSON_AddStringToObject(pickup_address, "country", "");
    cJSON_AddStringToObject(pickup_address, "postCode", "");
    cJSON_AddItemToObject(body, "pickupAddress", pickup_address);

    cJSON *dropoff_address = cJSON_CreateObject();
    cJSON_AddStringToObject(dropoff_address, "address", "");
    cJSON_AddStringToObject(dropoff_address, "streetNo", "");
    cJSON_AddStringToObject(dropoff_address, "streetName", "");
    cJSON_AddStringToObject(dropoff_address, "suburb", "");
    cJSON_AddStringToObject(dropoff_address, "state", "");
    cJSON_AddStringToObject(dropoff_address, "country", "");
    cJSON_AddStringToObject(dropoff_address, "postCode", "");
    cJSON_AddItemToObject(body, "dropOffAddress", dropoff_address);

    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

esp_err_t trip_sync_add_job(const char *customer_name) {
    int32_t local_id = session_store_get_active_local_trip_id();
    if (local_id <= 0) {
        ESP_LOGE(TAG, "add_job: no active local trip");
        return ESP_ERR_INVALID_STATE;
    }
    if (session_store_get_active_server_job_id() > 0) {
        ESP_LOGI(TAG, "add_job: trip #%ld already has a server job id — skipping", (long)local_id);
        return ESP_OK;
    }

    double pickup_lat = 0.0, pickup_lon = 0.0;
    if (!fare_calc_get_pickup_location(&pickup_lat, &pickup_lon)) {
        ESP_LOGW(TAG, "add_job: no pickup GPS fix recorded yet — sending 0,0 (server may reject this, matching "
                 "Android's own \"Invalid pickup location\" guard for the same case)");
    }

    strlcpy(s_customer_name, customer_name ? customer_name : "", sizeof(s_customer_name));

    char *body_str = _build_add_job_json(s_customer_name, pickup_lat, pickup_lon);
    if (!body_str) {
        ESP_LOGE(TAG, "add_job: failed to build request JSON (out of memory?)");
        return ESP_ERR_NO_MEM;
    }

    char *resp = malloc(TRIP_SYNC_BUFFER_SIZE);
    if (!resp) {
        ESP_LOGE(TAG, "add_job: malloc(%d) for response buffer FAILED — out of heap (see 'mem')", TRIP_SYNC_BUFFER_SIZE);
        free(body_str);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "ADD JOB — local trip #%ld, pickup=%.6f,%.6f", (long)local_id, pickup_lat, pickup_lon);
    int status = 0;
    esp_err_t err = _request_with_reauth(API_METHOD_POST, EP_ADD_JOB, body_str, resp, TRIP_SYNC_BUFFER_SIZE, &status);
    free(body_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADD JOB failed — no response from server (network/TLS issue, see above)");
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return err;
    }

    cJSON *json = cJSON_Parse(resp);
    if (!json) {
        ESP_LOGE(TAG, "ADD JOB: response is not valid JSON (HTTP %d) — raw: %.200s", status, resp);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    cJSON *success = cJSON_GetObjectItemCaseSensitive(json, "success");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(json, "message");
    cJSON *data    = cJSON_GetObjectItemCaseSensitive(json, "data");
    cJSON *job_id  = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "id") : NULL;

    if (!cJSON_IsTrue(success) || !cJSON_IsNumber(job_id) || job_id->valuedouble <= 0) {
        ESP_LOGE(TAG, "ADD JOB FAILED (HTTP %d): %s", status,
                 cJSON_IsString(message) ? message->valuestring : "(no message — check raw response)");
        cJSON_Delete(json);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    int64_t server_job_id = (int64_t)job_id->valuedouble;
    session_store_set_active_trip(local_id, server_job_id);
    ESP_LOGI(TAG, "ADD JOB OK \xE2\x9C\x93 — local #%ld -> server job id %lld", (long)local_id, (long long)server_job_id);
    cJSON_Delete(json);
    free(resp);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  2. Trips update — POST taxis-api/api/Trips
// ═══════════════════════════════════════════════════════════════
static void _add_time_frames_and_paths(cJSON *body, int64_t server_job_id) {
    time_frame_t frames[FARE_CALC_MAX_TIME_FRAMES];
    int count = fare_calc_get_time_frames(frames, FARE_CALC_MAX_TIME_FRAMES);

    const tariff_t *tariff = fare_calc_get_active_tariff();
    double threshold_m = tariff->distance_rate_range_km * 1000.0;   // tariff-level tier boundary, doc 150 §9.2

    cJSON *time_frames = cJSON_CreateArray();
    cJSON *paths        = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        const time_frame_t *f = &frames[i];

        cJSON *tf = cJSON_CreateObject();
        cJSON_AddNumberToObject(tf, "tripId", (double)server_job_id);
        char start_str[32], end_str[32];
        _iso8601_utc(f->start_time, start_str, sizeof(start_str));
        cJSON_AddStringToObject(tf, "startTime", start_str);
        if (f->end_time > 0) {
            _iso8601_utc(f->end_time, end_str, sizeof(end_str));
            cJSON_AddStringToObject(tf, "endTime", end_str);
        } else {
            cJSON_AddStringToObject(tf, "endTime", "");
        }
        cJSON_AddNumberToObject(tf, "status", (double)f->status);
        cJSON_AddNumberToObject(tf, "distanceInMeter", f->distance_m);
        cJSON_AddNumberToObject(tf, "timeInSec", (int)f->gps_active_time_s);
        cJSON_AddNumberToObject(tf, "tempTimeInSec", (int)f->gps_inactive_time_s);
        cJSON_AddNumberToObject(tf, "totalFare", f->total_fare_cents);
        cJSON_AddNumberToObject(tf, "distanceFare", f->distance_fare_cents);
        cJSON_AddNumberToObject(tf, "timeFare", f->time_fare_cents);
        cJSON_AddNumberToObject(tf, "startOdometerMeters", f->start_odometer_m);
        if (f->has_threshold) {
            cJSON_AddNumberToObject(tf, "distanceInMeterAtThreshold", f->distance_at_threshold_m);
        } else {
            cJSON_AddNullToObject(tf, "distanceInMeterAtThreshold");   // mirrors Android's nullable field, doc 150 §9.2
        }
        if (threshold_m > 0.0) {
            cJSON_AddNumberToObject(tf, "thresholdDistanceInMeters", threshold_m);
        } else {
            cJSON_AddNullToObject(tf, "thresholdDistanceInMeters");
        }
        cJSON_AddItemToArray(time_frames, tf);

        for (int p = 0; p < f->point_count; p++) {
            const time_frame_point_t *pt = &f->points[p];
            cJSON *path = cJSON_CreateObject();
            cJSON_AddNumberToObject(path, "tripId", (double)server_job_id);
            cJSON_AddNumberToObject(path, "latitude", pt->lat);
            cJSON_AddNumberToObject(path, "longitude", pt->lon);
            char at_str[32];
            _iso8601_utc(pt->at, at_str, sizeof(at_str));
            cJSON_AddStringToObject(path, "time", at_str);
            cJSON_AddItemToArray(paths, path);
        }
    }

    cJSON_AddItemToObject(body, "timeFrames", time_frames);
    cJSON_AddItemToObject(body, "paths", paths);
}

static char *_build_trip_update_json(int meter_status_ordinal) {
    int64_t server_job_id = session_store_get_active_server_job_id();
    const tariff_t *tariff = fare_calc_get_active_tariff();
    fare_calc_snapshot_t snap;
    fare_calc_get_snapshot(&snap);

    double pickup_lat = 0.0, pickup_lon = 0.0;
    fare_calc_get_pickup_location(&pickup_lat, &pickup_lon);

    cJSON *body = cJSON_CreateObject();
    cJSON *trip = cJSON_CreateObject();
    cJSON_AddNumberToObject(trip, "tripId", (double)server_job_id);
    cJSON_AddNumberToObject(trip, "jobId", (double)server_job_id);
    cJSON_AddStringToObject(trip, "requestedPickup", "");
    cJSON_AddStringToObject(trip, "company", "");
    cJSON_AddStringToObject(trip, "customerName", s_customer_name);
    cJSON_AddStringToObject(trip, "phoneNo", "");
    cJSON_AddStringToObject(trip, "pickupLocation", "");
    cJSON_AddNumberToObject(trip, "pickup_lat", pickup_lat);
    cJSON_AddNumberToObject(trip, "pickup_lng", pickup_lon);
    cJSON_AddStringToObject(trip, "dropOffLocation", "");
    cJSON_AddNumberToObject(trip, "dropOff_lat", 0.0);
    cJSON_AddNumberToObject(trip, "dropOff_lng", 0.0);
    cJSON_AddBoolToObject(trip, "isFixedRate", false);
    cJSON_AddNullToObject(trip, "fixedRate");
    cJSON_AddBoolToObject(trip, "isFixedFareAutomatic", false);
    cJSON_AddNullToObject(trip, "fixedRateAutomatic");
    cJSON_AddNumberToObject(trip, "extras", snap.extras_cents);
    cJSON_AddNumberToObject(trip, "maximumFareExtrasRecorded", snap.extras_cents);
    cJSON_AddNumberToObject(trip, "tariffId", (double)tariff->tariff_id);
    cJSON_AddStringToObject(trip, "tariffName", tariff->name);
    cJSON_AddNumberToObject(trip, "flagFall", tariff->flag_fall_cents);
    cJSON_AddNumberToObject(trip, "distanceRate", tariff->distance_rate_cents_per_km);
    cJSON_AddNumberToObject(trip, "timeRate", tariff->time_rate_cents_per_min);
    cJSON_AddNumberToObject(trip, "distance", snap.distance_km * 1000.0);
    cJSON_AddNumberToObject(trip, "duration", (snap.total_active_time_s + snap.total_inactive_time_s) / 60.0);
    cJSON_AddNumberToObject(trip, "totalFare", snap.total_fare_cents);
    cJSON_AddNumberToObject(trip, "meterStatus", meter_status_ordinal);
    cJSON_AddNullToObject(trip, "googleMapTollDetection");
    cJSON_AddStringToObject(trip, "deviceAppVersion", TAXIMETER_APP_VERSION);
    cJSON_AddNumberToObject(trip, "minChargeInCents", 0);
    cJSON_AddNumberToObject(trip, "maxChargeInCents", 0);
    cJSON_AddStringToObject(trip, "summary", "");
    cJSON_AddStringToObject(trip, "geometry", "");
    cJSON_AddItemToObject(body, "trip", trip);

    // Structured breakdowns not tracked per-item on this device yet
    // (fare_calc.c only keeps a lump extras/special-fares total) — sent
    // empty; the lump total above still correctly reflects in
    // trip.extras/trip.totalFare. See trip_sync.h's scope note.
    cJSON_AddItemToObject(body, "specialFares", cJSON_CreateArray());
    cJSON_AddItemToObject(body, "visitedTolls", cJSON_CreateArray());
    cJSON_AddItemToObject(body, "eventLogs", cJSON_CreateArray());

    _add_time_frames_and_paths(body, server_job_id);

    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

esp_err_t trip_sync_update_trip(int meter_status_ordinal) {
    int64_t server_job_id = session_store_get_active_server_job_id();
    if (server_job_id <= 0) {
        ESP_LOGE(TAG, "update_trip: no server job id yet — call trip_sync_add_job() first");
        return ESP_ERR_INVALID_STATE;
    }

    char *body_str = _build_trip_update_json(meter_status_ordinal);
    if (!body_str) {
        ESP_LOGE(TAG, "update_trip: failed to build request JSON (out of memory?)");
        return ESP_ERR_NO_MEM;
    }

    char *resp = malloc(TRIP_SYNC_BUFFER_SIZE);
    if (!resp) {
        ESP_LOGE(TAG, "update_trip: malloc(%d) for response buffer FAILED — out of heap (see 'mem')", TRIP_SYNC_BUFFER_SIZE);
        free(body_str);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "UPDATE TRIP — server job id %lld, meterStatus=%d, body=%u bytes",
             (long long)server_job_id, meter_status_ordinal, (unsigned)strlen(body_str));
    api_client_log_heap("before Trips POST");
    int status = 0;
    esp_err_t err = _request_with_reauth(API_METHOD_POST, EP_TRIP_UPDATE, body_str, resp, TRIP_SYNC_BUFFER_SIZE, &status);
    api_client_log_heap("after Trips POST");
    free(body_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UPDATE TRIP failed — no response from server");
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return err;
    }

    cJSON *json = cJSON_Parse(resp);
    cJSON *success = json ? cJSON_GetObjectItemCaseSensitive(json, "success") : NULL;
    cJSON *message = json ? cJSON_GetObjectItemCaseSensitive(json, "message") : NULL;
    bool ok = cJSON_IsTrue(success);
    if (!ok) {
        ESP_LOGE(TAG, "UPDATE TRIP FAILED (HTTP %d): %s", status,
                 cJSON_IsString(message) ? message->valuestring : "(no message — check raw response)");
    } else {
        ESP_LOGI(TAG, "UPDATE TRIP OK \xE2\x9C\x93 (HTTP %d)", status);
    }
    if (json) cJSON_Delete(json);
    free(resp);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ok ? ESP_OK : ESP_FAIL;
}

// ═══════════════════════════════════════════════════════════════
//  3. SaveJobFares — POST taxis-api/api/Job/SaveJobFares
// ═══════════════════════════════════════════════════════════════
static char *_build_save_fares_json(bool is_cancelled) {
    int64_t server_job_id = session_store_get_active_server_job_id();
    const tariff_t *tariff = fare_calc_get_active_tariff();
    fare_calc_snapshot_t snap;
    fare_calc_get_snapshot(&snap);

    double dropoff_lat = 0.0, dropoff_lon = 0.0;
    const gps_data_t *g = gps_client_get_latest();
    if (g && g->has_fix) { dropoff_lat = g->lat; dropoff_lon = g->lon; }

    char mac[18];
    _get_mac_address(mac, sizeof(mac));

    char end_time[32];
    _iso8601_utc(time(NULL), end_time, sizeof(end_time));
    char start_time[32];
    _iso8601_utc(snap.started_at, start_time, sizeof(start_time));

    double duration_min = (snap.total_active_time_s + snap.total_inactive_time_s) / 60.0;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "macAddress", mac);
    cJSON_AddNumberToObject(body, "jobId", (double)server_job_id);
    cJSON_AddNumberToObject(body, "latitude", dropoff_lat);
    cJSON_AddNumberToObject(body, "longitude", dropoff_lon);
    cJSON_AddStringToObject(body, "address", "");
    cJSON_AddStringToObject(body, "streetNo", "");
    cJSON_AddStringToObject(body, "streetName", "");
    cJSON_AddStringToObject(body, "suburb", "");
    cJSON_AddStringToObject(body, "state", "");
    cJSON_AddStringToObject(body, "postCode", "");
    cJSON_AddStringToObject(body, "country", "");
    cJSON_AddStringToObject(body, "tripStartTime", start_time);
    cJSON_AddStringToObject(body, "tripEndTime", end_time);
    cJSON_AddNumberToObject(body, "tarifId", (double)tariff->tariff_id);
    cJSON_AddStringToObject(body, "toCity", "");
    cJSON_AddNumberToObject(body, "toLatitude", 0.0);
    cJSON_AddNumberToObject(body, "toLongitude", 0.0);
    cJSON_AddNumberToObject(body, "tripDistance", snap.distance_km * 1000.0);
    cJSON_AddNumberToObject(body, "tripDuration", duration_min);
    cJSON_AddNumberToObject(body, "distanceFare", snap.distance_fare_cents);
    cJSON_AddNumberToObject(body, "durationFare", snap.time_fare_cents + snap.gps_inactive_fare_cents);
    cJSON_AddNumberToObject(body, "totalFare", snap.total_fare_cents);
    cJSON_AddBoolToObject(body, "isFixedRate", false);
    cJSON_AddNullToObject(body, "fixedRate");
    cJSON_AddBoolToObject(body, "isFixedFareAutomatic", false);
    cJSON_AddNullToObject(body, "fixedRateAutomatic");
    cJSON_AddNumberToObject(body, "fareExtras", snap.extras_cents);
    cJSON_AddNumberToObject(body, "maximumFareExtrasRecorded", snap.extras_cents);
    cJSON_AddItemToObject(body, "specialFares", cJSON_CreateArray());
    cJSON_AddItemToObject(body, "visitedTolls", cJSON_CreateArray());
    cJSON_AddNullToObject(body, "googleMapTollDetection");
    cJSON_AddItemToObject(body, "jobEventLogs", cJSON_CreateArray());
    cJSON_AddStringToObject(body, "dropOffTime", end_time);
    cJSON_AddBoolToObject(body, "isCreditCardPayment", false);
    cJSON_AddStringToObject(body, "city", "");
    cJSON_AddStringToObject(body, "placeId", "");
    cJSON_AddNumberToObject(body, "distance", 0.0);
    cJSON_AddNumberToObject(body, "duration", 0.0);
    cJSON_AddNumberToObject(body, "minChargeInCents", 0);
    cJSON_AddNumberToObject(body, "maxChargeInCents", 0);
    cJSON_AddStringToObject(body, "summary", "");
    cJSON_AddStringToObject(body, "geometry", "");
    cJSON_AddBoolToObject(body, "isCancelled", is_cancelled);

    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

esp_err_t trip_sync_save_fares(bool is_cancelled) {
    int64_t server_job_id = session_store_get_active_server_job_id();
    if (server_job_id <= 0) {
        ESP_LOGE(TAG, "save_fares: no server job id yet — call trip_sync_add_job() first");
        return ESP_ERR_INVALID_STATE;
    }

    fare_calc_snapshot_t snap_for_log;
    fare_calc_get_snapshot(&snap_for_log);

    char *body_str = _build_save_fares_json(is_cancelled);
    if (!body_str) {
        ESP_LOGE(TAG, "save_fares: failed to build request JSON (out of memory?)");
        return ESP_ERR_NO_MEM;
    }

    char *resp = malloc(TRIP_SYNC_BUFFER_SIZE);
    if (!resp) {
        ESP_LOGE(TAG, "save_fares: malloc(%d) for response buffer FAILED — out of heap (see 'mem')", TRIP_SYNC_BUFFER_SIZE);
        free(body_str);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "SAVE JOB FARES — server job id %lld, total=%.2f\xC2\xA2, cancelled=%s",
             (long long)server_job_id, snap_for_log.total_fare_cents, is_cancelled ? "yes" : "no");
    int status = 0;
    esp_err_t err = _request_with_reauth(API_METHOD_POST, EP_SAVE_JOB_FARES, body_str, resp, TRIP_SYNC_BUFFER_SIZE, &status);
    free(body_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SAVE JOB FARES failed — no response from server");
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return err;
    }

    cJSON *json = cJSON_Parse(resp);
    cJSON *success = json ? cJSON_GetObjectItemCaseSensitive(json, "success") : NULL;
    cJSON *message = json ? cJSON_GetObjectItemCaseSensitive(json, "message") : NULL;
    // "already updated" is treated as success — matches Android's own
    // UpdateCompletedTripToServerUseCase (doc 149 §2.2), which tolerates
    // a resync landing on an already-settled job.
    bool already_updated = cJSON_IsString(message) && strstr(message->valuestring, "already updated") != NULL;
    bool ok = cJSON_IsTrue(success) || already_updated;

    if (!ok) {
        ESP_LOGE(TAG, "SAVE JOB FARES FAILED (HTTP %d): %s", status,
                 cJSON_IsString(message) ? message->valuestring : "(no message — check raw response)");
    } else {
        ESP_LOGI(TAG, "SAVE JOB FARES OK \xE2\x9C\x93 (HTTP %d)%s", status, already_updated ? " (already updated)" : "");
    }
    if (json) cJSON_Delete(json);
    free(resp);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ok ? ESP_OK : ESP_FAIL;
}

// ═══════════════════════════════════════════════════════════════
//  FULL SEQUENCE (doc 149 §2.2's "canonical trip-finished sequence")
// ═══════════════════════════════════════════════════════════════
esp_err_t trip_sync_run_full_sequence(bool is_cancelled) {
    int32_t local_id = session_store_get_active_local_trip_id();
    if (local_id <= 0) {
        strlcpy(s_last_sync_note, "no active trip", sizeof(s_last_sync_note));
        s_last_sync_ok = false;
        s_last_sync_at = time(NULL);
        ESP_LOGW(TAG, "run_full_sequence: no active local trip — nothing to sync");
        return ESP_ERR_INVALID_STATE;
    }

    if (session_store_get_active_server_job_id() <= 0) {
        esp_err_t err = trip_sync_add_job(s_customer_name);
        if (err != ESP_OK) {
            strlcpy(s_last_sync_note, "AddJob failed", sizeof(s_last_sync_note));
            s_last_sync_ok = false;
            s_last_sync_at = time(NULL);
            return err;
        }
    }

    int meter_status = is_cancelled ? METER_STATUS_CANCELLED
                        : (fare_calc_is_running() ? METER_STATUS_STARTED : METER_STATUS_FINALIZED);
    esp_err_t err = trip_sync_update_trip(meter_status);
    if (err != ESP_OK) {
        strlcpy(s_last_sync_note, "Trips update failed", sizeof(s_last_sync_note));
        s_last_sync_ok = false;
        s_last_sync_at = time(NULL);
        return err;
    }

    err = trip_sync_save_fares(is_cancelled);
    strlcpy(s_last_sync_note, err == ESP_OK ? "OK" : "SaveJobFares failed", sizeof(s_last_sync_note));
    s_last_sync_ok = (err == ESP_OK);
    s_last_sync_at = time(NULL);
    return err;
}

// ═══════════════════════════════════════════════════════════════
//  4. Trip history — POST taxis-api/api/Job/GetAllBySearch (doc 188)
// ═══════════════════════════════════════════════════════════════
static void _extract_string(cJSON *parent, const char *key, char *out, size_t out_size) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (cJSON_IsString(v) && v->valuestring) strlcpy(out, v->valuestring, out_size);
}

int trip_sync_fetch_history(int page_number, int page_size,
                             trip_history_item_t *out, int max_count,
                             int *out_total_count) {
    if (out_total_count) *out_total_count = 0;
    if (!out || max_count <= 0) return 0;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "pageNumber", page_number);
    cJSON_AddNumberToObject(req, "pageSize", page_size);
    // Exact filter Android's GetTripHistoryUseCase.kt uses — completed
    // trips only, same set its own History tab shows.
    cJSON_AddStringToObject(req, "search", "Dropedoff");
    cJSON_AddStringToObject(req, "searchColumn", "Status");
    char *body_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body_str) {
        ESP_LOGE(TAG, "fetch_history: failed to build request JSON (out of memory?)");
        return 0;
    }

    // doc 188: TRIP_HISTORY_BUFFER_SIZE, NOT TRIP_SYNC_BUFFER_SIZE — a
    // real captured response truncated at 8192 (a full page of JobDto
    // records is much bigger than the other 3 calls' small responses).
    char *resp = malloc(TRIP_HISTORY_BUFFER_SIZE);
    if (!resp) {
        ESP_LOGE(TAG, "fetch_history: malloc(%d) for response buffer FAILED — out of heap (see 'mem')", TRIP_HISTORY_BUFFER_SIZE);
        free(body_str);
        return 0;
    }

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "GET TRIP HISTORY — page %d, pageSize %d, search=Dropedoff/Status", page_number, page_size);
    int status = 0;
    esp_err_t err = _request_with_reauth(API_METHOD_POST, EP_JOB_SEARCH, body_str, resp, TRIP_HISTORY_BUFFER_SIZE, &status);
    free(body_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET TRIP HISTORY failed — no response from server");
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return 0;
    }

    cJSON *json = cJSON_Parse(resp);
    free(resp);
    if (!json) {
        ESP_LOGE(TAG, "GET TRIP HISTORY: response is not valid JSON (HTTP %d)", status);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return 0;
    }

    cJSON *success = cJSON_GetObjectItemCaseSensitive(json, "success");
    cJSON *data    = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsTrue(success) || !cJSON_IsObject(data)) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(json, "message");
        ESP_LOGE(TAG, "GET TRIP HISTORY FAILED (HTTP %d): %s", status,
                 cJSON_IsString(message) ? message->valuestring : "(no message — check raw response)");
        cJSON_Delete(json);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return 0;
    }

    cJSON *total_count = cJSON_GetObjectItemCaseSensitive(data, "totalCount");
    if (out_total_count && cJSON_IsNumber(total_count)) *out_total_count = (int)total_count->valuedouble;

    cJSON *items = cJSON_GetObjectItemCaseSensitive(data, "items");
    int count = 0;
    cJSON *item = NULL;
    if (cJSON_IsArray(items)) {
        cJSON_ArrayForEach(item, items) {
            if (count >= max_count) break;
            trip_history_item_t *dst = &out[count];
            memset(dst, 0, sizeof(*dst));

            cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
            dst->id = cJSON_IsNumber(id) ? (int64_t)id->valuedouble : 0;
            if (dst->id <= 0) continue;   // JobDto.id is non-nullable in Android — no id, no usable row

            cJSON *jstatus = cJSON_GetObjectItemCaseSensitive(item, "status");
            dst->status = cJSON_IsNumber(jstatus) ? (int)jstatus->valuedouble : 0;

            cJSON *pickup = cJSON_GetObjectItemCaseSensitive(item, "pickup");
            if (cJSON_IsObject(pickup)) {
                _extract_string(pickup, "pickupTime", dst->pickup_time, sizeof(dst->pickup_time));
                cJSON *addr = cJSON_GetObjectItemCaseSensitive(pickup, "address");
                if (cJSON_IsObject(addr)) _extract_string(addr, "addressLine1", dst->pickup_address, sizeof(dst->pickup_address));
            }

            cJSON *dropoff = cJSON_GetObjectItemCaseSensitive(item, "dropOff");
            if (cJSON_IsObject(dropoff)) {
                _extract_string(dropoff, "dropOffTime", dst->dropoff_time, sizeof(dst->dropoff_time));
                cJSON *addr = cJSON_GetObjectItemCaseSensitive(dropoff, "address");
                if (cJSON_IsObject(addr)) _extract_string(addr, "addressLine1", dst->dropoff_address, sizeof(dst->dropoff_address));
            }

            cJSON *fares = cJSON_GetObjectItemCaseSensitive(item, "totalFares");
            dst->total_fares = cJSON_IsNumber(fares) ? fares->valuedouble : 0.0;

            _extract_string(item, "fromCity", dst->from_city, sizeof(dst->from_city));
            _extract_string(item, "toCity", dst->to_city, sizeof(dst->to_city));

            // Cache the FULL raw JobDto for this row under the usual
            // trips_<id>.json convention — a history-row tap then opens
            // in the existing trip_json_viewer_show(id) unchanged, with
            // no extra per-row network round-trip (this response already
            // carried the whole JobDto body for every item on the page).
            char *item_str = cJSON_PrintUnformatted(item);
            if (item_str) {
                rest_api_storage_write((int)dst->id, item_str, strlen(item_str));
                free(item_str);
            }

            count++;
        }
    }

    ESP_LOGI(TAG, "GET TRIP HISTORY OK \xE2\x9C\x93 (HTTP %d) — %d item(s), totalCount=%d", status, count,
             out_total_count ? *out_total_count : -1);
    cJSON_Delete(json);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return count;
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════
static void _show_help(void) {
    printf("\n  sync now       Force AddJob/Trips/SaveJobFares for the active trip, right now\n");
    printf("  sync status    Local trip id / server job id / last sync result\n");
    printf("  sync help\n\n");
}

// Runs on bg_worker's persistent 8KB-stack task, never on the "serial_cmd"
// task itself — a full AddJob+Trips+SaveJobFares sequence is 3 sequential
// HTTPS/TLS calls, far too much mbedTLS call depth for a small task stack
// (same reasoning as every other HTTPS-calling serial command in this
// codebase — see auth_client.c's/reference_data.c's matching comments).
static bool _sync_now_job(void *arg) {
    (void)arg;
    return trip_sync_run_full_sequence(false) == ESP_OK;
}

bool trip_sync_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "sync", 4) != 0) return false;

    const char *p = line + 4;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) {
        _show_help();
    } else if (strcmp(p, "now") == 0) {
        if (!bg_worker_submit_fn(_sync_now_job, NULL, NULL, NULL)) {
            ESP_LOGW(TAG, "sync now: background worker busy — try again shortly");
        } else {
            ESP_LOGI(TAG, "sync now: queued on background worker — watch below for ADD JOB/UPDATE TRIP/SAVE JOB FARES logs");
        }
    } else if (strcmp(p, "status") == 0) {
        ESP_LOGI(TAG, "Local trip id: %ld | Server job id: %lld%s",
                 (long)session_store_get_active_local_trip_id(),
                 (long long)session_store_get_active_server_job_id(),
                 session_store_get_active_server_job_id() > 0 ? "" : " (not synced)");
        if (s_last_sync_at > 0) {
            ESP_LOGI(TAG, "Last sync attempt: %lds ago — %s (%s)",
                     (long)(time(NULL) - s_last_sync_at), s_last_sync_ok ? "OK" : "FAILED", s_last_sync_note);
        } else {
            ESP_LOGI(TAG, "Last sync attempt: never");
        }
    } else {
        printf("Unknown 'sync' subcommand. Type 'sync help'.\n");
    }
    return true;
}
