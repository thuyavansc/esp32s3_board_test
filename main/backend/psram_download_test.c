#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/sha256.h"
#include "config.h"
#include "ram_test.h"
#include "psram_download_test.h"

static const char *TAG = "psram_dl";

// Same helper/rotating-buffer approach as ram_test.c's own _fmt() — kept as
// its own local copy (this module has no shared-utility header to pull it
// from), formats a byte count as "X.X KB" (under 1MB) or "X.XX MB" (1MB+).
static const char *_fmt(long bytes) {
    static char buf[4][24];
    static int  idx = 0;
    idx = (idx + 1) % 4;
    if (bytes < 0) bytes = 0;
    if ((size_t)bytes < 1024u * 1024u) {
        snprintf(buf[idx], sizeof(buf[idx]), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf[idx], sizeof(buf[idx]), "%.2f MB", bytes / (1024.0 * 1024.0));
    }
    return buf[idx];
}

// snprintf()'s two hex digits per byte, over a 32-byte SHA-256 digest,
// plus the NUL terminator.
static void _bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_len) {
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < out_len; i++) {
        pos += snprintf(out + pos, out_len - pos, "%02x", bytes[i]);
    }
    out[pos] = '\0';
}

// ── Event-handler-driven download context ──────────────────────────
// Same shape as ota_client.c's _ota_write_ctx_t/_ota_download_event_handler
// (writes each chunk into the destination AS IT ARRIVES, from inside the
// event callback), except the destination here is a PSRAM buffer instead
// of a flash OTA partition. This deliberately replaces the manual
// esp_http_client_open()+fetch_headers()+read() approach — that one is
// the ONE thing about this module that differed from every other proven-
// working HTTPS call in this codebase (trips_api.c's real download,
// ota_client.c's real download both use esp_http_client_perform() + an
// event handler; this was the only module using the manual read() API).
typedef struct {
    uint8_t *buf;            // allocated lazily, once Content-Length is known (ON_HEADER)
    int      capacity;       // = the real, live Content-Length — never guessed/hardcoded
    int      bytes_written;
    bool     alloc_failed;
    bool     over_ceiling;
    bool     write_overflow; // server sent more than its own declared Content-Length
} _dl_ctx_t;

static esp_err_t _dl_event_handler(esp_http_client_event_t *evt) {
    _dl_ctx_t *ctx = (_dl_ctx_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        // Capture Content-Length the same moment esp_http_client itself
        // parses it off the wire — this is the "live, real, never-hardcoded"
        // size this project's whole config.h philosophy insists on. Headers
        // always complete before any HTTP_EVENT_ON_DATA fires, so the buffer
        // is guaranteed ready before the first byte of body arrives.
        if (strcasecmp(evt->header_key, "Content-Length") == 0 && !ctx->buf) {
            long len = atol(evt->header_value);
            ESP_LOGI(TAG, "  Server-reported size (live Content-Length header): %ld bytes (%s)", len, _fmt(len));
            if (len <= 0) {
                return ESP_OK; // leave ctx->buf NULL; caught after perform() returns
            }
            if ((size_t)len > PSRAM_DL_TEST_MAX_BYTES) {
                ESP_LOGE(TAG, "  FAIL — reported size %ld bytes (%s) exceeds the %u byte (%s) safety ceiling "
                              "(PSRAM_DL_TEST_MAX_BYTES, config.h) — refusing to allocate",
                         len, _fmt(len), (unsigned)PSRAM_DL_TEST_MAX_BYTES, _fmt(PSRAM_DL_TEST_MAX_BYTES));
                ctx->over_ceiling = true;
                return ESP_OK;
            }
            ctx->buf = (uint8_t *)heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
            if (!ctx->buf) {
                ESP_LOGE(TAG, "  FAIL — heap_caps_malloc(%ld bytes / %s, MALLOC_CAP_SPIRAM) returned NULL", len, _fmt(len));
                ctx->alloc_failed = true;
                return ESP_OK;
            }
            ctx->capacity = (int)len;
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (!ctx->buf || ctx->bytes_written + evt->data_len > ctx->capacity) {
            ctx->write_overflow = true;
            return ESP_FAIL;
        }
        memcpy(ctx->buf + ctx->bytes_written, evt->data, evt->data_len);
        ctx->bytes_written += evt->data_len;
    }
    return ESP_OK;
}

void psram_download_test_run(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "PSRAM DOWNLOAD TEST — real HTTPS download, buffered whole in PSRAM");
    ESP_LOGI(TAG, "  URL: %s", PSRAM_DL_TEST_URL);
    ram_test_log_snapshot("before download");

    _dl_ctx_t ctx = {0};

    esp_http_client_config_t http_cfg = {
        .url               = PSRAM_DL_TEST_URL,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = PSRAM_DL_TEST_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 4096,
        .keep_alive_enable = false,
        .user_agent        = "ESP32-PSRAM-DL-Test/1.0",
        .event_handler     = _dl_event_handler,
        .user_data         = &ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "  FAIL — esp_http_client_init() returned NULL");
        return;
    }

    // esp_http_client_perform() drives the ENTIRE request/response cycle in
    // one blocking call (connect -> send -> headers -> body), invoking
    // _dl_event_handler() for each header line and each body chunk as they
    // arrive — the exact same proven mechanism trips_api.c's and
    // ota_client.c's own real downloads already use successfully in this
    // codebase, instead of this module's previous manual open()+
    // fetch_headers()+read() approach.
    esp_err_t perform_err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (ctx.over_ceiling || ctx.alloc_failed) {
        ram_test_log_snapshot("after failed allocation");
        if (ctx.buf) heap_caps_free(ctx.buf);
        esp_http_client_cleanup(client);
        return;
    }

    if (perform_err != ESP_OK || status != 200 || ctx.write_overflow || !ctx.buf) {
        ESP_LOGE(TAG, "  FAIL — esp_http_client_perform(): %s | HTTP status %d | bytes received %d (%s)%s",
                 esp_err_to_name(perform_err), status, ctx.bytes_written, _fmt(ctx.bytes_written),
                 ctx.write_overflow ? " | server sent MORE than its own declared Content-Length" : "");
        if (ctx.buf) heap_caps_free(ctx.buf);
        esp_http_client_cleanup(client);
        return;
    }

    if (ctx.bytes_written != ctx.capacity) {
        ESP_LOGE(TAG, "  FAIL — incomplete download: got %d bytes (%s) / %d bytes (%s) (connection dropped early?)",
                 ctx.bytes_written, _fmt(ctx.bytes_written), ctx.capacity, _fmt(ctx.capacity));
        heap_caps_free(ctx.buf);
        esp_http_client_cleanup(client);
        return;
    }

    ESP_LOGI(TAG, "  Download complete: %d bytes (%s) / %d bytes (%s) received",
             ctx.bytes_written, _fmt(ctx.bytes_written), ctx.capacity, _fmt(ctx.capacity));
    ram_test_log_snapshot("after download, before verify");

    // ── Verification — size (already implicit: bytes_written == capacity
    // above) and SHA-256 integrity, against the known-good hash from the
    // real manifest response (config.h's PSRAM_DL_TEST_SHA256). ──
    uint8_t digest[32];
    mbedtls_sha256(ctx.buf, (size_t)ctx.capacity, digest, 0 /* 0 = SHA-256, not SHA-224 */);
    char digest_hex[65];
    _bytes_to_hex(digest, sizeof(digest), digest_hex, sizeof(digest_hex));

    bool sha_ok = (strcasecmp(digest_hex, PSRAM_DL_TEST_SHA256) == 0);
    ESP_LOGI(TAG, "  Expected SHA-256: %s", PSRAM_DL_TEST_SHA256);
    ESP_LOGI(TAG, "  Actual   SHA-256: %s", digest_hex);
    if (sha_ok) {
        ESP_LOGI(TAG, "  Integrity: PASS — size AND SHA-256 both match, %d bytes (%s) held in one PSRAM buffer", ctx.capacity, _fmt(ctx.capacity));
    } else {
        ESP_LOGE(TAG, "  Integrity: FAIL — SHA-256 mismatch (downloaded data is not what was expected)");
    }

    heap_caps_free(ctx.buf);
    ram_test_log_snapshot("after free");

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "══════════════════════════════════════");
}
