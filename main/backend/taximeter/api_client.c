/**
 * api_client.c — Shared authenticated HTTPS + JSON helper
 *
 * See api_client.h for the full design rationale. This file has exactly
 * one HTTP-performing function (_perform) that both public entry points
 * funnel through, so the TLS setup / heap logging / error classification
 * logic exists in one place, not duplicated per caller.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "api_client.h"
#include "session_store.h"

static const char *TAG = "api_client";

// ── Response sink — either a bounded buffer or an open FILE*, never both ──
typedef struct {
    char   *buf;         // NULL if writing to a file instead
    size_t  buf_size;
    size_t  buf_used;
    bool    buf_truncated;

    FILE   *file;         // NULL if writing to a buffer instead
    size_t  file_bytes;
    bool    file_write_error;
} response_sink_t;

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    response_sink_t *sink = (response_sink_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "  HTTP transport error event");
            break;

        case HTTP_EVENT_ON_DATA:
            if (evt->data_len <= 0 || !sink) break;

            if (sink->file) {
                size_t written = fwrite(evt->data, 1, evt->data_len, sink->file);
                sink->file_bytes += written;
                if (written != (size_t)evt->data_len) {
                    ESP_LOGE(TAG, "  File write short (%u of %d bytes) — disk full or SPIFFS error?",
                             (unsigned)written, evt->data_len);
                    sink->file_write_error = true;
                    return ESP_FAIL;
                }
            } else if (sink->buf) {
                size_t space = (sink->buf_size > sink->buf_used + 1) ? (sink->buf_size - sink->buf_used - 1) : 0;
                size_t to_copy = ((size_t)evt->data_len < space) ? (size_t)evt->data_len : space;
                if (to_copy > 0) {
                    memcpy(sink->buf + sink->buf_used, evt->data, to_copy);
                    sink->buf_used += to_copy;
                }
                if (to_copy < (size_t)evt->data_len) {
                    sink->buf_truncated = true;   // logged once, after perform(), by the caller
                }
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

void api_client_log_heap(const char *when) {
    size_t free_heap     = esp_get_free_heap_size();
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "  Heap %-22s free=%u KB | largest free block=%u KB",
             when, (unsigned)(free_heap / 1024), (unsigned)(largest_block / 1024));
}

// ── The one place every request actually goes through ───────────────
static esp_err_t _perform(api_method_t method, const char *path, const char *json_body,
                           bool use_auth, response_sink_t *sink, int *out_status) {
    if (out_status) *out_status = 0;

    char url[256];
    snprintf(url, sizeof(url), "https://%s%s", TAXIMETER_API_HOST, path);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .method            = (method == API_METHOD_POST) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms        = TAXIMETER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   // TLS CA verification
        .event_handler     = _http_event_handler,
        .user_data         = sink,
        .buffer_size       = API_SMALL_BUFFER_SIZE,
        // buffer_size_tx (the OUTGOING request buffer — separate from
        // buffer_size above, which is only for the response) defaults to
        // a size too small to fit a large "Authorization: Bearer <JWT>"
        // header once the token is a few hundred bytes — this is the
        // real cause of "HTTP_HEADER: Buffer length is small to fit
        // all the headers" warnings seen on every authenticated call.
        // Same size as the response buffer is generous headroom.
        .buffer_size_tx    = API_SMALL_BUFFER_SIZE,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "✗ %s %s — esp_http_client_init() FAILED (out of memory?)",
                 method == API_METHOD_POST ? "POST" : "GET", path);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ESP32-TaxiMeter/1.0");

    if (use_auth) {
        // Must stay >= session_store's access_token[] size (session_store.c)
        // or a real (large) token gets truncated again right here even
        // after being stored correctly.
        char token[1536];
        if (session_store_get_access_token(token, sizeof(token)) && token[0] != '\0') {
            char auth_header[1600];   // "Bearer " (7) + token + null
            snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
            esp_http_client_set_header(client, "Authorization", auth_header);
        } else {
            ESP_LOGW(TAG, "  use_auth=true but no access token is stored yet — sending without it");
        }
    }

    if (json_body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, json_body, strlen(json_body));
    }

    ESP_LOGI(TAG, "→ %s %s", method == API_METHOD_POST ? "POST" : "GET", path);
    api_client_log_heap("before perform()");
    esp_err_t err = esp_http_client_perform(client);
    api_client_log_heap("after perform()");

    int status = esp_http_client_get_status_code(client);
    if (out_status) *out_status = status;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "\xE2\x86\x90 HTTP %d (%s %s)", status,
                 method == API_METHOD_POST ? "POST" : "GET", path);
    } else {
        // Classify the failure so the serial log answers "why" without
        // needing to cross-reference esp-tls internals every time.
        ESP_LOGE(TAG, "\xE2\x9C\x97 %s %s FAILED: %s (err=0x%04x)",
                 method == API_METHOD_POST ? "POST" : "GET", path, esp_err_to_name(err), err);
        if (err == ESP_ERR_HTTP_CONNECT) {
            ESP_LOGE(TAG, "  ESP_ERR_HTTP_CONNECT with no HTTP status usually means DNS/TCP/TLS");
            ESP_LOGE(TAG, "  never completed — check WiFi is connected and 'largest free block' above");
            ESP_LOGE(TAG, "  against a healthy run (heap fragmentation is a common cause on this board).");
        } else if (err == ESP_ERR_HTTP_EAGAIN) {
            ESP_LOGE(TAG, "  Timed out waiting for a response — check server reachability / TAXIMETER_HTTP_TIMEOUT_MS.");
        }
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t api_client_request(api_method_t method, const char *path, const char *json_body,
                              bool use_auth, char *out_buf, size_t out_buf_size, int *out_status) {
    if (!out_buf || out_buf_size == 0) return ESP_ERR_INVALID_ARG;
    out_buf[0] = '\0';

    response_sink_t sink = { .buf = out_buf, .buf_size = out_buf_size };
    esp_err_t err = _perform(method, path, json_body, use_auth, &sink, out_status);
    sink.buf[sink.buf_used] = '\0';

    if (sink.buf_truncated) {
        ESP_LOGW(TAG, "  Response TRUNCATED at %u bytes (buffer is %u bytes) — increase out_buf_size "
                 "if this response needs to be read in full.", (unsigned)sink.buf_used, (unsigned)out_buf_size);
    }
    return err;
}

// Large file-backed responses (PublicHolidays' size is the one that's
// actually hit this) are the only calls in this codebase big enough for
// a single corrupted TLS record mid-transfer to matter — MBEDTLS_ERR_
// SSL_INVALID_RECORD (-0x7200), seen on a weak WiFi link: one bad packet
// and the whole TLS record is rejected, there's no partial-record
// recovery in TLS. Small calls (Login/Driver/Tariffs/etc.) haven't shown
// this because they're far less exposed — fewer packets, fewer chances
// for one to get corrupted. A retry is the standard, correct mitigation
// for a transient transport error like this — each attempt reopens
// dest_path with "w" (truncate), so a partial write from a failed
// attempt never bleeds into the next.
#define API_FILE_FETCH_MAX_ATTEMPTS 3
#define API_FILE_FETCH_RETRY_DELAY_MS 800

esp_err_t api_client_request_to_file(api_method_t method, const char *path, const char *json_body,
                                      bool use_auth, const char *dest_path, int *out_status) {
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= API_FILE_FETCH_MAX_ATTEMPTS; attempt++) {
        FILE *fp = fopen(dest_path, "w");   // "w" truncates — a clean slate every attempt
        if (!fp) {
            ESP_LOGE(TAG, "  Could not open '%s' for writing (SPIFFS not mounted, or directory missing?)", dest_path);
            return ESP_FAIL;
        }

        response_sink_t sink = { .file = fp };
        err = _perform(method, path, json_body, use_auth, &sink, out_status);
        fclose(fp);

        bool ok = (err == ESP_OK) && !sink.file_write_error && (!out_status || *out_status == 200);
        if (ok) {
            ESP_LOGI(TAG, "  Wrote %u bytes to %s%s", (unsigned)sink.file_bytes, dest_path,
                     attempt > 1 ? " (succeeded on retry)" : "");
            return ESP_OK;
        }

        // Never leave a partial/failed-fetch file behind to be mistaken
        // for real stored data.
        if (remove(dest_path) == 0) {
            ESP_LOGW(TAG, "  Removed incomplete/failed response file: %s", dest_path);
        }
        if (err == ESP_OK && sink.file_write_error) err = ESP_FAIL;

        if (attempt < API_FILE_FETCH_MAX_ATTEMPTS) {
            ESP_LOGW(TAG, "  Attempt %d/%d failed for %s — retrying in %dms (likely a weak-WiFi transport glitch, not a real failure)",
                     attempt, API_FILE_FETCH_MAX_ATTEMPTS, path, API_FILE_FETCH_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(API_FILE_FETCH_RETRY_DELAY_MS));
        }
    }

    ESP_LOGE(TAG, "  All %d attempts failed for %s — giving up", API_FILE_FETCH_MAX_ATTEMPTS, path);
    return err;
}
