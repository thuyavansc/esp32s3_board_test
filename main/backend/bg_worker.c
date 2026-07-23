/**
 * bg_worker.c — ONE persistent background task for Trip FETCH
 *
 * See bg_worker.h for the full "why" — short version: creating a new
 * FreeRTOS task on every button tap needs a large contiguous free heap
 * block at an unpredictable time; this creates the task's stack exactly
 * once, at boot, and reuses it forever via a queue.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "bg_worker.h"
#include "backend/trips_api.h"

static const char *TAG = "bgworker";

typedef struct {
    bg_job_type_t     type;
    int               arg;
    bg_job_done_cb_t  done_cb;
    void             *user_data;
} bg_job_t;

static QueueHandle_t   s_queue = NULL;
static volatile bool   s_busy  = false;

static void _worker_task(void *arg) {
    bg_job_t job;
    while (1) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        s_busy = true;

        bool success = false;
        switch (job.type) {
            case BG_JOB_TRIP_FETCH:
                ESP_LOGI(TAG, "Running job: TRIP FETCH #%d", job.arg);
                success = (trips_api_fetch(job.arg) == ESP_OK);
                break;
        }
        ESP_LOGI(TAG, "Job finished: %s", success ? "OK" : "FAILED");

        s_busy = false;
        if (job.done_cb) job.done_cb(success, job.arg, job.user_data);
    }
}

void bg_worker_init(void) {
    s_queue = xQueueCreate(4, sizeof(bg_job_t));
    // 8 KB — sized for the larger of the two jobs (HTTPS/TLS trip fetch
    // needs more headroom than a plain TCP socket send). Allocated ONCE,
    // here, as early in boot as possible — see bg_worker.h.
    xTaskCreate(_worker_task, "bg_worker", 8192, NULL, 3, NULL);
    ESP_LOGI(TAG, "Background worker task started (persistent, one-time stack allocation)");
}

bool bg_worker_submit(bg_job_type_t type, int arg, bg_job_done_cb_t done_cb, void *user_data) {
    if (!s_queue) return false;
    bg_job_t job = { .type = type, .arg = arg, .done_cb = done_cb, .user_data = user_data };
    return xQueueSend(s_queue, &job, 0) == pdTRUE;  // never blocks the caller (LVGL thread)
}

bool bg_worker_is_busy(void) {
    return s_busy || (s_queue != NULL && uxQueueMessagesWaiting(s_queue) > 0);
}
