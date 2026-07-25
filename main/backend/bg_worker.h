#pragma once
// ================================================================
// bg_worker.h — ONE persistent background task shared by every
// "run this on a background task, not the LVGL thread" action
// (Trip FETCH, plus any TaxiMeter backend HTTPS call via
// bg_worker_submit_fn() — login, setup, duty, reference-data fetch).
//
// Superset of the original esp32s3_board diagnostic version — ported
// from esp32_display_taxi_meter's bg_worker.c/h (which also had
// BG_JOB_GPS_SEND for its iStartek socket module). GPS_SEND is dropped
// here — this project has no socket_client.c compiled in (excluded,
// see main/CMakeLists.txt) — but BG_JOB_FN (the generic function-pointer
// job) is kept, since the TaxiMeter backend modules (auth_client,
// setup_client, duty_client, reference_data, trip_manager) all need it.
//
// WHY THIS EXISTS: creating a new FreeRTOS task on every button tap or
// serial command needs a large contiguous free heap block (6-8KB) at an
// unpredictable point in runtime, after WiFi/display/SPIFFS have
// already consumed and fragmented most of it — which is exactly what
// was failing ("xTaskCreate FAILED ... out of memory?"). See
// docs/TestFunctionalities/display/46_2026-07-09_memory_and_flash_analysis.md
// for the full diagnosis.
//
// This module creates ONE task, ONCE, as early in boot as possible
// (call bg_worker_init() before wifi_init() in app_main — see there)
// while the heap is still at its largest. After that, submitting a
// job is just a queue-send — no heap allocation, ever again, no
// matter how fragmented the heap gets later.
//
// GENERIC FUNCTION-POINTER JOBS:
// The original esp32s3_board design used one enum value per job type,
// switch-dispatched inside the worker task. That doesn't scale once
// auth/setup/duty/trip-sync/reference-data calls all need the same "run
// on the persistent worker task" treatment — bg_worker_submit_fn() lets
// any module register its own job body as a plain function pointer
// instead of bg_worker.c growing a case for every caller. The original
// enum-based bg_worker_submit() is kept as-is (rest_api_storage.c's "api
// get" command already uses it) — both job kinds share the same queue
// and worker task.
// ================================================================
#include <stdbool.h>

typedef enum {
    BG_JOB_TRIP_FETCH,  // arg = trip_id
    BG_JOB_FN,          // generic function-pointer job — see bg_worker_submit_fn()
} bg_job_type_t;

// Called with the job's result — ALWAYS from the worker task's own
// context, never the LVGL thread. Only write to a plain result struct
// here; let an lv_timer on the LVGL thread pick it up and do the
// actual UI update (same pattern trip_screen.c already uses).
typedef void (*bg_job_done_cb_t)(bool success, int arg, void *user_data);

// Starts the persistent worker task. Call ONCE, as early as possible
// in app_main() — see the note above for why "early" matters.
void bg_worker_init(void);

// Queues a job. Never blocks the caller. Returns false immediately if
// a job is already queued/running and this one can't be accepted yet
// (the queue holds a small backlog, but jobs are meant to run one at a
// time anyway).
bool bg_worker_submit(bg_job_type_t type, int arg, bg_job_done_cb_t done_cb, void *user_data);

// A job body for bg_worker_submit_fn() — runs on the worker task, must
// never call any LVGL function (see the threading note above). Return
// true/false for success/failure; that value is passed straight
// through to done_cb.
typedef bool (*bg_job_fn_t)(void *arg);
typedef void (*bg_job_fn_done_cb_t)(bool success, void *arg, void *user_data);

// Queues a generic function-pointer job — the mechanism every TaxiMeter
// backend module (auth_client, setup_client, duty_client, reference_data,
// trip_manager) uses to run its own HTTPS calls on the shared worker
// task instead of each needing its own enum entry here. Never blocks the
// caller; returns false immediately if the queue is full.
bool bg_worker_submit_fn(bg_job_fn_t fn, void *arg, bg_job_fn_done_cb_t done_cb, void *user_data);

// True if a job is currently queued or running.
bool bg_worker_is_busy(void);
