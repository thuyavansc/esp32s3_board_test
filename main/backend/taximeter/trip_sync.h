#pragma once
// ================================================================
// trip_sync.h — Trip-to-server sync (AddJob -> Trips -> SaveJobFares)
//
// WHAT THIS MODULE DOES:
//   Builds and sends the three server calls that make a locally-run
//   trip visible to the backend, in the exact sequence the Android
//   reference app uses (docs/TestFunctionalities/esp32s3_board/
//   calculations-impl/149_2026-07-26_android_feature_endpoint_map_and_
//   data_flow.md §2.2 — "the canonical trip-finished sequence"):
//
//     1. trip_sync_add_job()     -> POST Job/AddJobByDriver
//        Reserves a server job id for a trip that only has a LOCAL id
//        so far. Must succeed before either call below can run (they
//        need the server job id). Mirrors session_store's own
//        server_job_id<=0 = "not yet synced" convention.
//     2. trip_sync_update_trip() -> POST Trips
//        Pushes the full trip snapshot: identity, tariff, totals,
//        every TimeFrame (fare_calc_get_time_frames()) and every GPS
//        point recorded in them.
//     3. trip_sync_save_fares()  -> POST Job/SaveJobFares
//        The closing/settlement record — final fare breakdown.
//
// IDENTITY (D1, doc 151 §7.1): this device logs in with a username/
// password and holds a Bearer token — the Android app's "driver"
// identity (UserDetails.IS_DRIVER == true), NOT the AppKey/"device"
// identity. So these calls are the "Job/*"+"Trips" driver-variant
// endpoints (config.h), authenticated the same way every other
// TaxiMeter API call already is (api_client.c's use_auth=true).
//
// TOKEN EXPIRY (D1's follow-on risk, doc 151 §7.1): an unattended shift
// can outlive the Bearer token's lifetime. Every call in this module
// goes through _request_with_reauth() (trip_sync.c), which re-logs-in
// once (using config.h's AUTH_TEST_USERNAME/PASSWORD) and retries
// exactly once if the server responds 401 — the ESP32 equivalent of
// Android's AuthInterceptor auto-refresh-on-401. Confined to this
// module rather than added to the shared api_client.c, to keep that
// already-proven file unchanged (lower risk on a codebase that can't
// be compile-tested at plan time).
//
// SCOPE — deliberately NOT implemented this pass (documented, not
// missed — see docs/TestFunctionalities/esp32s3_board/
// calculations-impl/151_..._esp32_vs_android_gap_analysis_and_
// implementation_plan.md for the full list):
//   - Pickup / ChangeStatus calls (EP_PICKUP / EP_CHANGE_STATUS_FMT,
//     config.h) — their request DTOs (InputPickupDto especially)
//     weren't verified field-by-field against the Android source in
//     this pass, unlike AddJob/Trips/SaveJobFares which were. Endpoints
//     are defined and ready; wiring them in is a small follow-up once
//     that DTO is confirmed, not guessed at here.
//   - Cancel/Recall sync — out of scope for this pass (no cancel
//     workflow exists in trip_manager.c yet either).
//   - Per-point incremental "synced" flag — every update_trip() call
//     resends the WHOLE current path set (not just newly-added points,
//     unlike Android's `.filter { !it.synced }`) — a v1 simplification,
//     acceptable at this trip/point scale (FARE_CALC_MAX_TIME_FRAMES/
//     FARE_CALC_MAX_POINTS_PER_FRAME cap the payload size anyway).
//   - Structured specialFares[]/visitedTolls[] breakdown — ESP32 only
//     tracks a lump extras/special-fares total today (fare_calc.c), so
//     these arrays are sent empty; the LUMP total is still correctly
//     included in the trip's overall totalFare either way.
//
// SERIAL COMMANDS:
//   sync now      -> force the full AddJob/Trips/SaveJobFares sequence
//                    for the currently active trip, right now
//   sync status   -> local trip id / server job id / last sync result
//   sync help
// ================================================================
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Same ordinals as Android's MeterStatus enum (doc 149) — trip_manager.c
// passes one of these to trip_sync_update_trip()/is used to build the
// SaveJobFares "isCancelled" flag; trip_sync.c does not infer lifecycle
// state on its own.
typedef enum {
    METER_STATUS_NOT_STARTED = 0,
    METER_STATUS_STARTED     = 1,
    METER_STATUS_WAITING     = 2,
    METER_STATUS_PAUSED      = 3,
    METER_STATUS_STOPPED     = 4,
    METER_STATUS_FINALIZED   = 5,
    METER_STATUS_CANCELLED   = 6,
    METER_STATUS_RECALL      = 7,
} meter_status_ordinal_t;

// Reserve a server job id for the currently active LOCAL trip.
// no-ops (returns ESP_ERR_INVALID_STATE) if there's no active local
// trip, or if it already has a server job id (already added). Updates
// session_store's active-trip server_job_id on success.
esp_err_t trip_sync_add_job(const char *customer_name);

// Push the current full trip snapshot (trip/tariff identity, totals,
// every TimeFrame + its GPS points) to POST Trips. meter_status_ordinal
// is the caller's own Android-MeterStatus-ordinal choice (Started=1,
// Waiting=2, Paused=3, Stopped=4, Finalized=5 — trip_manager.c decides
// which applies; trip_sync.c doesn't infer lifecycle state on its own).
// Requires an existing server job id (call trip_sync_add_job() first)
// — returns ESP_ERR_INVALID_STATE otherwise.
esp_err_t trip_sync_update_trip(int meter_status_ordinal);

// Push the closing settlement record (POST Job/SaveJobFares).
// Requires an existing server job id — same precondition as
// trip_sync_update_trip().
esp_err_t trip_sync_save_fares(bool is_cancelled);

// Runs the full sequence for the active trip, in order, stopping early
// if an earlier step fails (AddJob if not yet done -> update_trip ->
// save_fares) — this is what "sync now" and trip_manager's finalize
// path both call. Safe to call even if AddJob already succeeded
// earlier (skips straight to update_trip/save_fares in that case).
// Runs synchronously on the CALLING task — callers on a small-stack
// task (the serial/tick tasks) MUST route this through
// bg_worker_submit_fn(), never call it directly (matches every other
// HTTPS-calling module in this codebase — see fare_calc.h's threading
// note and duty_client.c's own comment for why).
esp_err_t trip_sync_run_full_sequence(bool is_cancelled);

// Serial command handler ("sync ...")
bool trip_sync_process_command(const char *line);

// ═══════════════════════════════════════════════════════════════
//  Trip history list (doc 188) — POST Job/GetAllBySearch, the REAL
//  data source behind Android's TripHistoryFragment11 (confirmed
//  field-by-field against features/trip_sync/usecases/
//  GetTripHistoryUseCase.kt and features/available_trip/dtos/JobDto.kt
//  — NOT guessed). Android auto-loads this on screen open, filtered to
//  search="Dropedoff"/searchColumn="Status" (only completed trips) —
//  same filter used here, so the ESP32 History tab shows the same set
//  Android's own History tab would, with no trip-ID typing needed.
//  This is a DIFFERENT server call from rest_api_storage_fetch()'s
//  single-trip GET Trips/{id} (that one still exists — it's the Manual
//  Fetch tab now, doc 188).
// ═══════════════════════════════════════════════════════════════
typedef struct {
    int64_t id;
    int     status;               // JobDto.status (Android's JobStatus ordinal)
    char    pickup_time[32];      // JobDto.pickup.pickupTime (ISO8601)
    char    dropoff_time[32];     // JobDto.dropOff.dropOffTime
    double  total_fares;          // JobDto.totalFares (dollars — matches the server's Double, same unit Android displays)
    char    from_city[32];        // JobDto.fromCity
    char    to_city[32];          // JobDto.toCity
    char    pickup_address[64];   // JobDto.pickup.address.addressLine1
    char    dropoff_address[64];  // JobDto.dropOff.address.addressLine1
} trip_history_item_t;

// Fetches one page of the driver's completed-trip history. Blocks on
// HTTPS — call from bg_worker only, never the LVGL thread (same rule as
// every other call in this module). Returns the number of items
// written into `out` (up to max_count, 0 on any failure).
// `out_total_count`, if non-NULL, receives the server's totalCount, for
// "there are more pages" paging.
//
// As a side effect, each returned item's full raw JSON is cached via
// rest_api_storage_write() under the same trips_<id>.json convention
// rest_api_storage_fetch() uses — a history row can be opened in the
// existing trip_json_viewer_show(id) unchanged, without a second
// network round-trip per row (Job/GetAllBySearch already returns the
// full JobDto body for every item in the page).
int trip_sync_fetch_history(int page_number, int page_size,
                             trip_history_item_t *out, int max_count,
                             int *out_total_count);
