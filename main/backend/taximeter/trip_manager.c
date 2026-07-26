/**
 * trip_manager.c — Local trip lifecycle
 *
 * See trip_manager.h for the full design and explicit out-of-scope list
 * (server sync, toll geofencing, mid-trip tariff switch — all later
 * phases).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"
#include "gps/gps_client.h"
#include "session_store.h"
#include "reference_data.h"
#include "fare_calc.h"
#include "trip_sync.h"
#include "bg_worker.h"
#include "trip_manager.h"

static const char *TAG = "trip";

#define TRIPS_DIR  "/spiffs" STORAGE_DIR "/trips"
// Flush the trip's persisted JSON snapshot to flash every this-many
// fare_calc ticks (10s at the 2s tick period) rather than every tick —
// bounds flash write wear.
#define PERSIST_EVERY_N_TICKS  5

// Queue a periodic Trips-update (not the full AddJob/SaveJobFares
// sequence — just an in-progress snapshot) every this-many ticks (60s
// at the 2s tick period, doc 151 §7.1 D5's sync-first pass). A blocking
// HTTPS POST every 2s would be excessive network/CPU load for a value
// (the live fare) that doesn't need second-by-second server visibility —
// 60s is a reasonable "eventually consistent" cadence, same spirit as
// Android's own requestSync()-on-state-change-plus-periodic-catch-up
// model (doc 149 §2.2), without trying to match its exact trigger points.
#define TRIP_SYNC_EVERY_N_TICKS  30

static char s_customer_name[32] = {0};
static int  s_tick_counter = 0;
static int  s_sync_tick_counter = 0;

// ── Persistence — one JSON file per trip, /spiffs/store/trips/trip_<id>.json ──
static void _ensure_dir(void) {
    struct stat st;
    if (stat(TRIPS_DIR, &st) != 0) mkdir(TRIPS_DIR, 0755);
}

static void _persist_trip_state(void) {
    int32_t local_id = session_store_get_active_local_trip_id();
    if (local_id <= 0) return;

    fare_calc_snapshot_t snap;
    fare_calc_get_snapshot(&snap);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "local_trip_id", local_id);
    cJSON_AddNumberToObject(j, "server_job_id", (double)session_store_get_active_server_job_id());
    cJSON_AddStringToObject(j, "customer_name", s_customer_name);
    cJSON_AddNumberToObject(j, "tariff_id", (double)snap.tariff_id);
    cJSON_AddStringToObject(j, "tariff_type", snap.tariff_type);
    cJSON_AddNumberToObject(j, "started_at", (double)snap.started_at);
    cJSON_AddBoolToObject(j, "is_running", snap.is_running);
    cJSON_AddBoolToObject(j, "is_paused", snap.is_paused);
    cJSON_AddNumberToObject(j, "distance_km", snap.distance_km);
    cJSON_AddNumberToObject(j, "flag_fall_cents", snap.flag_fall_cents);
    cJSON_AddNumberToObject(j, "distance_fare_cents", snap.distance_fare_cents);
    cJSON_AddNumberToObject(j, "time_fare_cents", snap.time_fare_cents);
    cJSON_AddNumberToObject(j, "gps_inactive_fare_cents", snap.gps_inactive_fare_cents);
    cJSON_AddNumberToObject(j, "extras_cents", snap.extras_cents);
    cJSON_AddNumberToObject(j, "special_fares_cents", snap.special_fares_cents);
    cJSON_AddNumberToObject(j, "total_fare_cents", snap.total_fare_cents);

    char *str = cJSON_Print(j);
    cJSON_Delete(j);
    if (!str) { ESP_LOGE(TAG, "persist: cJSON_Print failed (out of memory?)"); return; }

    _ensure_dir();
    char path[96];
    snprintf(path, sizeof(path), "%s/trip_%ld.json", TRIPS_DIR, (long)local_id);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        ESP_LOGE(TAG, "persist: could not open '%s' for writing", path);
    } else {
        fwrite(str, 1, strlen(str), fp);
        fclose(fp);
    }
    free(str);
}

// ═══════════════════════════════════════════════════════════════
//  SYNC — background-worker job wrappers (never called directly from
//  the tick task or the serial-command task — see trip_sync.h's
//  threading note; all HTTPS work must run on bg_worker's 8KB-stack
//  persistent task, same rule every other TaxiMeter backend module
//  already follows)
// ═══════════════════════════════════════════════════════════════
static int _current_meter_status_ordinal(void) {
    fare_calc_snapshot_t snap;
    fare_calc_get_snapshot(&snap);
    if (!snap.is_running) return METER_STATUS_STOPPED;
    if (snap.is_paused) return METER_STATUS_PAUSED;
    return METER_STATUS_STARTED;
}

static bool _job_add_job_on_start(void *arg) {
    (void)arg;
    return trip_sync_add_job(s_customer_name) == ESP_OK;
}

static bool _job_periodic_trip_update(void *arg) {
    (void)arg;
    if (session_store_get_active_server_job_id() <= 0) {
        // AddJob hasn't landed yet (still queued, or its earlier attempt
        // failed) — try it here too, matching Android's own ordering
        // (Trips/SaveJobFares always require a server job id first, doc
        // 149 §2.2). Harmless no-op if it's already in flight elsewhere.
        if (trip_sync_add_job(s_customer_name) != ESP_OK) return false;
    }
    return trip_sync_update_trip(_current_meter_status_ordinal()) == ESP_OK;
}

static bool _job_finalize_sync(void *arg) {
    (void)arg;
    esp_err_t err = trip_sync_run_full_sequence(false);
    if (err == ESP_OK) {
        session_store_clear_active_trip();
        ESP_LOGI(TAG, "TRIP FINALIZED \xE2\x9C\x93 and synced — ready for a new 'trip start'");
    } else {
        ESP_LOGW(TAG, "TRIP FINALIZE: sync did not fully succeed — active trip id kept so 'sync now' can retry later");
    }
    return err == ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  TICK TASK — owns fare_calc_tick(), periodic flash persistence, and
//  periodic background trip-sync
// ═══════════════════════════════════════════════════════════════
static void _trip_tick_task(void *arg) {
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(FARE_CALC_TICK_MS));

        if (!fare_calc_is_running()) continue;
        fare_calc_tick();

        if (++s_tick_counter >= PERSIST_EVERY_N_TICKS) {
            s_tick_counter = 0;
            _persist_trip_state();
        }

        if (++s_sync_tick_counter >= TRIP_SYNC_EVERY_N_TICKS) {
            s_sync_tick_counter = 0;
            if (!bg_worker_is_busy()) {
                if (!bg_worker_submit_fn(_job_periodic_trip_update, NULL, NULL, NULL)) {
                    ESP_LOGW(TAG, "periodic sync: background worker rejected the job — will retry next interval");
                }
            } else {
                ESP_LOGI(TAG, "periodic sync: background worker busy — skipping this interval, will retry next one");
            }
        }
    }
}

void trip_manager_init(void) {
    xTaskCreate(_trip_tick_task, "trip_tick", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "Trip manager ready — fare-calc tick task started (every %dms)", FARE_CALC_TICK_MS);
}

esp_err_t trip_manager_start_trip(const char *customer_name) {
    if (trip_manager_is_trip_active()) {
        ESP_LOGE(TAG, "start_trip: a trip is already running — stop it first");
        return ESP_ERR_INVALID_STATE;
    }

    char tariff_type[16];
    if (!session_store_get_tariff_type(tariff_type, sizeof(tariff_type))) {
        ESP_LOGE(TAG, "start_trip: no tariff type set — fetch reference data first ('ref fetch')");
        return ESP_ERR_INVALID_STATE;
    }

    const tariff_t *tariff = reference_data_find_tariff_by_time(tariff_type, time(NULL));
    if (!tariff) {
        ESP_LOGE(TAG, "start_trip: no tariff found for type '%s' — is reference data loaded? ('ref info')", tariff_type);
        return ESP_ERR_NOT_FOUND;
    }

    int32_t local_id = session_store_next_local_trip_id();
    session_store_set_active_trip(local_id, 0);   // server_job_id=0 — not yet synced
    strlcpy(s_customer_name, customer_name ? customer_name : "", sizeof(s_customer_name));
    s_tick_counter = 0;

    fare_calc_start(tariff);

    const gps_data_t *g = gps_client_get_latest();
    if (g && g->has_fix) {
        ESP_LOGI(TAG, "  Pickup location: %.6f, %.6f", g->lat, g->lon);
    } else {
        ESP_LOGW(TAG, "  No GPS fix yet at trip start — pickup location will be missing until one arrives");
    }

    // Unconditional "Levy" auto-charge, if reference data has it.
    const special_fare_t *levy = reference_data_find_special_fare_by_code("Levy");
    if (levy) {
        fare_calc_add_special_fare_cents(levy->fare_cents);
        ESP_LOGI(TAG, "  Auto-added special fare: %s (%.2f\xC2\xA2)", levy->name, levy->fare_cents);
    }

    ESP_LOGI(TAG, "TRIP #%ld STARTED — customer=\"%s\"", (long)local_id, s_customer_name);
    _persist_trip_state();

    // Fire-and-forget AddJob sync — matches Android's own StartTripUseCase
    // calling tripSyncManager.requestSync() right after starting (doc 149
    // §5). Not fatal if the worker is busy: the periodic sync (tick task,
    // TRIP_SYNC_EVERY_N_TICKS) and "sync now" both retry AddJob on their
    // own if it hasn't landed yet.
    if (!bg_worker_submit_fn(_job_add_job_on_start, NULL, NULL, NULL)) {
        ESP_LOGW(TAG, "  Background worker busy — AddJob sync skipped for now (periodic sync/'sync now' will retry)");
    }

    return ESP_OK;
}

esp_err_t trip_manager_stop_trip(void) {
    if (!trip_manager_is_trip_active()) {
        ESP_LOGW(TAG, "stop_trip: no trip is currently running");
        return ESP_ERR_INVALID_STATE;
    }
    fare_calc_stop();
    _persist_trip_state();
    ESP_LOGI(TAG, "TRIP #%ld STOPPED (not yet synced to server)",
             (long)session_store_get_active_local_trip_id());
    return ESP_OK;
}

void trip_manager_pause_trip(void)  { fare_calc_pause();  _persist_trip_state(); }
void trip_manager_resume_trip(void) { fare_calc_resume(); _persist_trip_state(); }

esp_err_t trip_manager_finalize_trip(void) {
    if (session_store_get_active_local_trip_id() <= 0) {
        ESP_LOGW(TAG, "finalize: no active trip to finalize");
        return ESP_ERR_INVALID_STATE;
    }

    if (trip_manager_is_trip_active()) {
        trip_manager_stop_trip();   // mirrors Android's FinalizeTripUseCase: stop first if not already stopped, doc 149 §5
    }

    if (!bg_worker_submit_fn(_job_finalize_sync, NULL, NULL, NULL)) {
        ESP_LOGW(TAG, "finalize: background worker busy — meter is stopped locally; run 'sync now' or 'trip finalize' again shortly");
        return ESP_OK;   // the local stop already succeeded — only the server sync is pending
    }

    ESP_LOGI(TAG, "TRIP #%ld FINALIZE queued — watch below for ADD JOB/UPDATE TRIP/SAVE JOB FARES logs",
             (long)session_store_get_active_local_trip_id());
    return ESP_OK;
}

void trip_manager_add_extras_cents(double cents) {
    if (!trip_manager_is_trip_active()) {
        ESP_LOGW(TAG, "add_extras: no trip is currently running");
        return;
    }
    fare_calc_add_extras(cents);
    ESP_LOGI(TAG, "Extras added: %.2f\xC2\xA2", cents);
    _persist_trip_state();
}

bool trip_manager_is_trip_active(void) {
    return fare_calc_is_running();
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════
static void _show_help(void) {
    printf("\n  trip start [customerName]   Start the meter (resolves current tariff)\n");
    printf("  trip stop                   Stop the meter\n");
    printf("  trip pause / trip resume    Pause/resume billing\n");
    printf("  trip extras <dollars>       Add a flat extras charge\n");
    printf("  trip finalize               Stop (if needed) + full AddJob/Trips/SaveJobFares sync\n");
    printf("  trip info                   Live fare breakdown\n");
    printf("  trip help                   Show this help\n\n");
}

static void _show_info(void) {
    fare_calc_snapshot_t snap;
    fare_calc_get_snapshot(&snap);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "TRIP INFO");
    ESP_LOGI(TAG, "  Local trip id:    %ld", (long)session_store_get_active_local_trip_id());
    ESP_LOGI(TAG, "  Server job id:    %lld%s", (long long)session_store_get_active_server_job_id(),
             session_store_get_active_server_job_id() > 0 ? "" : " (not synced)");
    ESP_LOGI(TAG, "  Customer:         %s", s_customer_name[0] ? s_customer_name : "(none)");
    ESP_LOGI(TAG, "  Running:          %s%s", snap.is_running ? "yes" : "no", snap.is_paused ? " (PAUSED)" : "");
    ESP_LOGI(TAG, "  Tariff:           #%lld type=%s", (long long)snap.tariff_id, snap.tariff_type);
    ESP_LOGI(TAG, "  GPS active:       %s | speed=%.1f km/h", snap.gps_active ? "yes" : "no", snap.speed_kmh);
    ESP_LOGI(TAG, "  Distance:         %.3f km", snap.distance_km);
    ESP_LOGI(TAG, "  ── Fare breakdown ──");
    ESP_LOGI(TAG, "  Flag fall:        %8.2f\xC2\xA2", snap.flag_fall_cents);
    ESP_LOGI(TAG, "  Distance fare:    %8.2f\xC2\xA2", snap.distance_fare_cents);
    ESP_LOGI(TAG, "  Time fare:        %8.2f\xC2\xA2", snap.time_fare_cents);
    ESP_LOGI(TAG, "  GPS-inactive fare:%8.2f\xC2\xA2", snap.gps_inactive_fare_cents);
    ESP_LOGI(TAG, "  Extras:           %8.2f\xC2\xA2", snap.extras_cents);
    ESP_LOGI(TAG, "  Special fares:    %8.2f\xC2\xA2", snap.special_fares_cents);
    ESP_LOGI(TAG, "  TOTAL:            %8.2f\xC2\xA2  ($%.2f)", snap.total_fare_cents, snap.total_fare_cents / 100.0);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

bool trip_manager_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "trip", 4) != 0) return false;

    const char *p = line + 4;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) {
        _show_help();
    } else if (strncmp(p, "start", 5) == 0) {
        char customer[32] = {0};
        sscanf(p + 5, "%31s", customer);
        trip_manager_start_trip(customer[0] ? customer : NULL);
    } else if (strcmp(p, "stop") == 0) {
        trip_manager_stop_trip();
    } else if (strcmp(p, "pause") == 0) {
        trip_manager_pause_trip();
    } else if (strcmp(p, "resume") == 0) {
        trip_manager_resume_trip();
    } else if (strncmp(p, "extras", 6) == 0) {
        double dollars = 0.0;
        if (sscanf(p + 6, "%lf", &dollars) == 1) {
            trip_manager_add_extras_cents(dollars * 100.0);
        } else {
            printf("Usage: trip extras <dollars>\n");
        }
    } else if (strcmp(p, "finalize") == 0) {
        trip_manager_finalize_trip();
    } else if (strcmp(p, "info") == 0) {
        _show_info();
    } else {
        printf("Unknown 'trip' subcommand. Type 'trip help'.\n");
    }
    return true;
}
