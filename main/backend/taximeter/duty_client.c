/**
 * duty_client.c — On-duty / off-duty
 *
 * See duty_client.h — local status flips immediately, the server call
 * is fire-and-forget via bg_worker (never blocks the caller/UI thread),
 * and its result is intentionally discarded, matching the Android
 * reference app's own GoOnDutyUseCase/GoOffDutyUseCase exactly.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "config.h"
#include "api_client.h"
#include "session_store.h"
#include "reference_data.h"
#include "bg_worker.h"
#include "duty_client.h"

static const char *TAG = "duty";

// ── Background job bodies — run on bg_worker's persistent task, never
//    on the caller's/LVGL thread ──
static bool _job_on_duty(void *arg) {
    (void)arg;   // unused — bg_job_fn_t always takes a void* even when a job needs no input
    char resp[256];
    int status = 0;
    esp_err_t err = api_client_request(API_METHOD_GET, EP_ON_DUTY, NULL, true, resp, sizeof(resp), &status);
    ESP_LOGI(TAG, "  OnDuty API call: %s (HTTP %d) — result is informational only, never blocks the driver",
             err == ESP_OK ? "reached server" : "failed to reach server", status);
    return true;   // always "success" from the worker's point of view — see file header
}

static bool _job_off_duty(void *arg) {
    (void)arg;
    char resp[256];
    int status = 0;
    esp_err_t err = api_client_request(API_METHOD_GET, EP_OFF_DUTY, NULL, true, resp, sizeof(resp), &status);
    ESP_LOGI(TAG, "  OffDuty API call: %s (HTTP %d) — result is informational only, never blocks the driver",
             err == ESP_OK ? "reached server" : "failed to reach server", status);
    return true;
}

// Refetch reference data once per on-duty session rather than a
// Firebase-style version-check system. Runs as its own queued
// bg_worker job (after the OnDuty call above) rather than inline here,
// since it makes 4 sequential HTTPS calls — too slow to run on
// whatever thread called duty_client_go_on_duty().
static bool _job_ref_fetch_if_stale(void *arg) {
    (void)arg;
    reference_data_fetch_all_if_stale();
    return true;
}

void duty_client_go_on_duty(void) {
    session_store_set_duty_status(DUTY_STATUS_ON_DUTY);
    ESP_LOGI(TAG, "ON DUTY (local status updated immediately)");

    if (!bg_worker_submit_fn(_job_on_duty, NULL, NULL, NULL)) {
        ESP_LOGW(TAG, "  Background worker busy — OnDuty server call skipped this time (local status is still correct)");
    }
    if (!bg_worker_submit_fn(_job_ref_fetch_if_stale, NULL, NULL, NULL)) {
        ESP_LOGW(TAG, "  Background worker busy — reference-data refresh skipped this time (run 'ref fetch' manually)");
    }
}

void duty_client_go_off_duty(void) {
    session_store_set_duty_status(DUTY_STATUS_OFF_DUTY);
    ESP_LOGI(TAG, "OFF DUTY (local status updated immediately)");

    if (!bg_worker_submit_fn(_job_off_duty, NULL, NULL, NULL)) {
        ESP_LOGW(TAG, "  Background worker busy — OffDuty server call skipped this time (local status is still correct)");
    }
}

bool duty_client_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "duty", 4) != 0) return false;

    const char *p = line + 4;
    while (*p == ' ') p++;

    if (strcmp(p, "on") == 0) {
        duty_client_go_on_duty();
    } else if (strcmp(p, "off") == 0) {
        duty_client_go_off_duty();
    } else if (*p == '\0' || strcmp(p, "info") == 0) {
        ESP_LOGI(TAG, "Duty status: %s", session_store_get_duty_status() == DUTY_STATUS_ON_DUTY ? "ON DUTY" : "OFF DUTY");
    } else {
        printf("Unknown 'duty' subcommand. Try: duty on | duty off | duty info\n");
    }
    return true;
}
