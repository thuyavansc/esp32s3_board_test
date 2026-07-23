#pragma once
// ================================================================
// bg_worker.h — ONE persistent background task for actions that must
// NOT run on the LVGL thread (currently just Trip FETCH).
//
// Ported from esp32_display_taxi_3's bg_worker.c/h, which shared this
// same task between GPS SEND and Trip FETCH — this project has no
// GPS/socket hardware or code at all, so BG_JOB_GPS_SEND and its
// backend/socket_client.h dependency were dropped during the port
// (not disabled behind a flag — genuinely out of scope here).
//
// WHY THIS EXISTS: trip_screen.c used to call xTaskCreate() fresh on
// every button tap. That requires FreeRTOS to find a large contiguous
// free block of general heap (6-8KB) at an unpredictable point in
// runtime, after WiFi/display/SPIFFS have already consumed and
// fragmented most of it — which is exactly what was failing
// ("xTaskCreate FAILED ... out of memory?"). See
// docs/TestFunctionalities/display/46_2026-07-09_memory_and_flash_analysis.md
// for the full diagnosis.
//
// This module creates ONE task, ONCE, as early in boot as possible
// (call bg_worker_init() before wifi_init() in app_main — see there)
// while the heap is still at its largest. After that, submitting a
// job is just a queue-send — no heap allocation, ever again, no
// matter how fragmented the heap gets later.
// ================================================================
#include <stdbool.h>

typedef enum {
    BG_JOB_TRIP_FETCH,  // arg = trip_id
} bg_job_type_t;

// Called with the job's result — ALWAYS from the worker task's own
// context, never the LVGL thread. Only write to a plain result struct
// here; let an lv_timer on the LVGL thread pick it up and do the
// actual UI update (same pattern gps_screen.c/trip_screen.c already use).
typedef void (*bg_job_done_cb_t)(bool success, int arg, void *user_data);

// Starts the persistent worker task. Call ONCE, as early as possible
// in app_main() — see the note above for why "early" matters.
void bg_worker_init(void);

// Queues a job. Never blocks the caller. Returns false immediately if
// a job is already queued/running and this one can't be accepted yet
// (the queue holds a small backlog, but the two current job types are
// meant to run one at a time anyway).
bool bg_worker_submit(bg_job_type_t type, int arg, bg_job_done_cb_t done_cb, void *user_data);

// True if a job is currently queued or running.
bool bg_worker_is_busy(void);
