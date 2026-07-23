#include <stdio.h>
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

// snprintf()'s two hex digits per byte, over a 32-byte SHA-256 digest,
// plus the NUL terminator.
static void _bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_len) {
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < out_len; i++) {
        pos += snprintf(out + pos, out_len - pos, "%02x", bytes[i]);
    }
    out[pos] = '\0';
}

void psram_download_test_run(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "PSRAM DOWNLOAD TEST — real HTTPS download, buffered whole in PSRAM");
    ESP_LOGI(TAG, "  URL: %s", PSRAM_DL_TEST_URL);
    ram_test_log_snapshot("before download");

    esp_http_client_config_t http_cfg = {
        .url               = PSRAM_DL_TEST_URL,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = PSRAM_DL_TEST_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size       = 4096,   // HTTP layer's own internal read-chunk size — unrelated to
                                     // the PSRAM destination buffer sized below from Content-Length
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "  FAIL — esp_http_client_init() returned NULL");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  FAIL — esp_http_client_open(): %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    // The CORRECT way to size the destination buffer: read the real,
    // live Content-Length the server actually reports — never trust a
    // hardcoded "sizeBytes" from a manifest, and never guess. This also
    // means a bigger real file (mentioned: up to ~3MB) is handled
    // automatically with no code change — only the safety ceiling below
    // is fixed.
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "  FAIL — server did not report a usable Content-Length (got %d)", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }
    ESP_LOGI(TAG, "  Server-reported size (live Content-Length): %d bytes", content_length);

    if ((size_t)content_length > PSRAM_DL_TEST_MAX_BYTES) {
        ESP_LOGE(TAG, "  FAIL — reported size %d bytes exceeds the %u byte safety ceiling "
                      "(PSRAM_DL_TEST_MAX_BYTES, config.h) — refusing to allocate",
                 content_length, (unsigned)PSRAM_DL_TEST_MAX_BYTES);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)content_length, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "  FAIL — heap_caps_malloc(%d, MALLOC_CAP_SPIRAM) returned NULL", content_length);
        ram_test_log_snapshot("after failed allocation");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // Read in small, fixed-size chunks (matching http_cfg.buffer_size above)
    // instead of requesting the entire remaining length in one call.
    //
    // Confirmed on real hardware (twice, reproducibly, same exact byte
    // offset both times — ruling out a random network glitch): requesting
    // "everything left" in a single esp_http_client_read() call collided
    // with bytes esp_http_client already buffers internally during
    // fetch_headers() (headers + the start of the body often arrive in the
    // same underlying read), producing esp-tls-mbedtls "-0x7100" (invalid
    // MAC) right at the transition from serving that already-buffered data
    // to pulling fresh bytes off the real connection.
    //
    // trips_api.c/ota_client.c never hit this because esp_http_client_perform()
    // + an event handler already reads in exactly this same small-chunk
    // shape internally — this loop just reproduces that proven-working
    // shape explicitly, instead of asking for the whole remainder at once.
    const int read_chunk = 4096; // matches http_cfg.buffer_size above
    int total_read = 0;
    while (total_read < content_length) {
        int want = content_length - total_read;
        if (want > read_chunk) want = read_chunk;
        int r = esp_http_client_read(client, (char *)(buf + total_read), want);
        if (r <= 0) {
            ESP_LOGE(TAG, "  FAIL — esp_http_client_read() returned %d after %d/%d bytes (connection dropped early?)",
                     r, total_read, content_length);
            heap_caps_free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return;
        }
        total_read += r;
    }
    ESP_LOGI(TAG, "  Download complete: %d/%d bytes received", total_read, content_length);
    ram_test_log_snapshot("after download, before verify");

    // ── Verification — size (already implicit: total_read == content_length
    // above) and SHA-256 integrity, against the known-good hash from the
    // real manifest response (config.h's PSRAM_DL_TEST_SHA256). ──
    uint8_t digest[32];
    mbedtls_sha256(buf, (size_t)content_length, digest, 0 /* 0 = SHA-256, not SHA-224 */);
    char digest_hex[65];
    _bytes_to_hex(digest, sizeof(digest), digest_hex, sizeof(digest_hex));

    bool sha_ok = (strcasecmp(digest_hex, PSRAM_DL_TEST_SHA256) == 0);
    ESP_LOGI(TAG, "  Expected SHA-256: %s", PSRAM_DL_TEST_SHA256);
    ESP_LOGI(TAG, "  Actual   SHA-256: %s", digest_hex);
    if (sha_ok) {
        ESP_LOGI(TAG, "  Integrity: PASS — size AND SHA-256 both match, %d real bytes held in one PSRAM buffer", content_length);
    } else {
        ESP_LOGE(TAG, "  Integrity: FAIL — SHA-256 mismatch (downloaded data is not what was expected)");
    }

    heap_caps_free(buf);
    ram_test_log_snapshot("after free");

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "══════════════════════════════════════");
}
