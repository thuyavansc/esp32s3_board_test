#include "config.h"

#if ENABLE_OTA

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_efuse.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "config.h"
#include "ota_client.h"
#include "version.h"
#include "nvs_state.h"

// ================================================================
// ota_client.c — speaks the COMPANY'S real backend contract directly
// (added 2026-07-13, doc 54): GET /api/Esp32Ota/Manifest, GET <url>
// (plain static file), POST /api/Esp32Ota/Report — field-for-field per
// docs/TestFunctionalities/ota-updates/ota-from-dotnet/Esp32-Ota-Module-Implementation.md.
// This REPLACES the project's own original /api/ota/check + /api/ota/status
// shape (doc 46/47) — that server-side code still exists (unused now, not
// deleted, see the local ota_update_server's /api/ota/* routes), but this
// file no longer calls it. The whole point of matching the real contract
// here first: switching between the LOCAL SvelteKit mirror, the REAL
// remote company server, and a LOCAL .NET server (doc 65) is now just
// changing OTA_SERVER_MODE (config.h, doc 56/65) and rebuilding —
// nothing about how requests are built or responses parsed needs to
// change either way (see _build_server_url() below).
// ================================================================

static const char *TAG = "ota_client";

// Filled in by ota_client_check_now(), printed by "ota status" — purely
// diagnostic, does not affect logic.
static char s_last_server_ver[24]  = "(none)";
static int  s_last_firmware_id     = -1;
static bool s_last_update_found    = false;

// Consecutive check-in/download failures — drives the periodic task's
// backoff (doc 52 §9). Reset to 0 on any check-in that gets a parseable
// 200 or 404 response — 404 ("no firmware configured") is an EXPECTED
// state per the company doc §4.4, not a failure.
static int s_consecutive_failures = 0;

// ─── Device identity — used as BOTH chipId and macAddress in Report
// bodies (the company doc's own §6.1 leaves the exact MAC format as an
// OPEN QUESTION — "confirm the canonical format already used in
// MobileStatuses... otherwise reports from registered devices land on
// orphan records." This uses the same uppercase-colon-separated
// convention this project has used all along; CONFIRM AGAINST THE REAL
// MobileStatus.MacAddress FORMAT before pointing this at the real server). ──
static void _device_id(char *out, size_t out_len) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ─── Response body capture — esp_http_client_perform() drains the
// response internally unless an event handler captures it DURING the
// call; reading afterward always returns nothing. ──
static char s_resp[1024];
static int  s_resp_len;

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        int space = (int)sizeof(s_resp) - 1 - s_resp_len;
        if (space > 0) {
            int n = evt->data_len < space ? evt->data_len : space;
            memcpy(s_resp + s_resp_len, evt->data, n);
            s_resp_len += n;
        }
    }
    return ESP_OK;
}

// ─── Forces a "http://" URL to "https://" in place, IF OTA_USE_HTTPS is
// active (config.h, doc 56/58) — added 2026-07-13 after diagnosing a real
// download failure against the company's real server: the Manifest's own
// "url" field isn't always trustworthy about scheme. That server is
// HTTPS-only (confirmed — the Manifest/Report calls all validate a real
// cert), but its "url" field sometimes still comes back as plain
// "http://...". Trusting that literally meant esp_https_ota() connected
// over HTTP, got silently 30x-redirected to HTTPS by the server/proxy,
// and had the connection reset mid-download on that redirected session —
// exactly the "esp-tls-mbedtls: read error / Failed to establish HTTP
// connection" failure seen in testing. Rewriting the scheme BEFORE ever
// starting the download (doc 61's manual esp_ota_write() download, or
// esp_https_ota_begin() before that) connects directly over HTTPS the
// first time, skipping the redirect (and whatever was breaking on it)
// entirely. No-op when OTA_USE_HTTPS is 0 (local HTTP test mirror) or the
// URL is already "https://". ──
// Bug fixed 2026-07-14 (doc 69): this used to check the compile-time
// OTA_USE_HTTPS macro, which is fixed to the PRIMARY target (Vercel,
// HTTPS) for the whole build. Once _check_and_download() could run
// against a SECOND, different-scheme target (the local HTTP fallback,
// doc 69), that macro was still always 1 — so this kept force-upgrading
// the local fallback's genuinely-http:// binary URL to https://, and the
// local dev server (HTTP only) obviously has nothing listening there,
// producing "mbedtls_ssl_handshake returned -0x7200" — a self-inflicted
// failure, not a real one. Now takes the CALLING target's actual
// use_https as a runtime parameter instead of trusting a build-wide macro.
static void _force_https_if_needed(char *url, size_t url_len, bool use_https) {
    if (use_https && strncmp(url, "http://", 7) == 0) {
        char rest[300];
        strlcpy(rest, url + 7, sizeof(rest));
        snprintf(url, url_len, "https://%s", rest);
        ESP_LOGW(TAG, "  Manifest gave http:// for the binary — forcing https:// (this target uses HTTPS): %s", url);
    }
}

// ─── Cert verification switch — added doc 65. Normally every request
// sets .crt_bundle_attach = esp_crt_bundle_attach (validates the server's
// certificate against ESP-IDF's bundled public CA list — the correct,
// secure behavior). OTA_SERVER_MODE 3 (local HTTPS .NET server, using
// ASP.NET Core's own self-signed dev certificate) is the ONE exception:
// that certificate is never going to be in any public CA bundle, so
// verification would always fail. Setting crt_bundle_attach to NULL here
// omits it from the esp_http_client config entirely — which, per
// sdkconfig.defaults' CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY (also gated
// behind CONFIG_ESP_TLS_INSECURE), makes esp-tls accept ANY certificate
// for THIS specific request, with no verification at all. This is the
// device-side equivalent of a browser's "proceed anyway" click on a
// self-signed-cert warning — there is no interactive prompt on a
// microcontroller. SECURITY: only ever active when OTA_SERVER_MODE == 3
// (config.h) — every other mode still sets the real crt_bundle_attach and
// gets full, normal certificate verification. See doc 65 for the full
// security note before ever considering this for a real deployment. ──
#if OTA_INSECURE_SKIP_CERT_VERIFY
  #define OTA_CRT_BUNDLE_ATTACH   NULL
#else
  #define OTA_CRT_BUNDLE_ATTACH   esp_crt_bundle_attach
#endif

// ─── Boot-log label for the active OTA_SERVER_MODE — moved up here
// (2026-07-14, doc 69) from ota_client_init() so _check_and_download()
// below can also use it for the primary target's log lines. ──
#if OTA_SERVER_MODE == 1
  #define OTA_SERVER_MODE_LABEL "REAL remote company server"
#elif OTA_SERVER_MODE == 2
  #define OTA_SERVER_MODE_LABEL "LOCAL .NET server, HTTP (doc 65)"
#elif OTA_SERVER_MODE == 3
  // Bug fixed 2026-07-14 (doc 66) — this case was missing entirely, so
  // mode 3 silently fell into the #else branch and logged the WRONG
  // label ("LOCAL SvelteKit test mirror") despite correctly using mode
  // 3's actual host/port/HTTPS settings. Cosmetic only — never affected
  // which server was actually contacted, only what the boot log said.
  #define OTA_SERVER_MODE_LABEL "LOCAL .NET server, HTTPS (doc 65)"
#elif OTA_SERVER_MODE == 4
  #define OTA_SERVER_MODE_LABEL "Vercel-hosted test server (doc 69)"
#else
  #define OTA_SERVER_MODE_LABEL "LOCAL SvelteKit test mirror"
#endif

static bool _is_all_digits(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

// ─── Builds a full request URL from `path` (which may already include a
// "?query=string") per config.h's OTA_SERVER_MODE/OTA_USE_HTTPS/
// OTA_SERVER_HOST/OTA_SERVER_PORT (doc 56/65) — the ONE place that switch
// actually takes effect. Every request this file makes goes through this,
// EXCEPT the binary download itself, whose URL comes straight from the
// manifest response's "url" field (already absolute — see
// ota_client_check_now()). Change OTA_SERVER_MODE in config.h and
// rebuild to switch between the local test mirror, the real remote
// server, or a locally-run .NET server — nothing in this function needs
// to change either way. ──
// ─── Parameterized version (added 2026-07-14, doc 69) — same URL-building
// logic, but takes host/port/https explicitly instead of reading the
// OTA_SERVER_MODE macros, so _check_and_download() below can build a URL
// for either the primary target OR the local fallback target. ──
static void _build_server_url_ex(char *out, size_t out_len, const char *host, int port, bool use_https, const char *path) {
    if (use_https) {
        snprintf(out, out_len, "https://%s:%d%s", host, port, path);
    } else {
        snprintf(out, out_len, "http://%s:%d%s", host, port, path);
    }
}

static void _build_server_url(char *out, size_t out_len, const char *path) {
    // Always includes the port explicitly (added doc 65) — mode 1 (real
    // server, port 443) works identically whether ":443" is written out
    // or not, but mode 3 (local .NET over HTTPS, non-standard port 7273)
    // NEEDS it — omitting it would silently default to 443, which is
    // wrong for a local dev server. One code path, correct for every mode.
    //
    // Goes through nvs_state_resolve_server() (added 2026-07-14, doc 72)
    // rather than the OTA_SERVER_* macros directly — that resolver returns
    // those exact macro values unchanged UNLESS config.h's
    // ENABLE_SERVER_URL_OVERRIDE is on AND an admin has set a serverUrl via
    // remote config, so behavior here is identical to before by default.
    char host[96];
    int port;
    bool use_https;
    nvs_state_resolve_server(host, sizeof(host), &port, &use_https);
    _build_server_url_ex(out, out_len, host, port, use_https, path);
}

// ═══════════════════════════════════════════════════════════════
//  REPORT — POST /api/Esp32Ota/Report
//  Status vocabulary per the company doc §2.3: "Downloading", "Installing",
//  "Installed" (magic — confirms + rotates version on the server),
//  "Failed", "RolledBack".
// ═══════════════════════════════════════════════════════════════
static void _report(int firmware_id, const char *target_version, int target_version_code,
                     const char *status, const char *message) {
    char device_id[24];
    _device_id(device_id, sizeof(device_id));
    char company_id[48], product_name[48];
    nvs_state_get_company(company_id, sizeof(company_id));
    nvs_state_get_product(product_name, sizeof(product_name));
    char my_version[24];
    version_get_string(my_version, sizeof(my_version));
    bool has_company = _is_all_digits(company_id);

    char body[640];
    snprintf(body, sizeof(body),
             "{\"companyId\":%s,\"chipId\":\"%s\",\"macAddress\":\"%s\",\"productName\":\"%s\","
             "\"currentVersion\":\"%s\",\"currentVersionCode\":%d,"
             "\"targetVersion\":\"%s\",\"targetVersionCode\":%d,"
             "\"firmwareId\":%d,\"status\":\"%s\",\"message\":\"%s\","
             "\"freeHeap\":%lu}",
             has_company ? company_id : "null",
             device_id, device_id, product_name,
             my_version, version_get_code(),
             target_version ? target_version : my_version, target_version_code,
             firmware_id, status, message ? message : "",
             (unsigned long)esp_get_free_heap_size());

    char url[160];
    _build_server_url(url, sizeof(url), OTA_REPORT_PATH);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        // Required for https:// (OTA_USE_HTTPS=1, config.h doc 56/65) —
        // harmless/unused when the URL is plain http:// (local testing).
        .crt_bundle_attach = OTA_CRT_BUNDLE_ATTACH,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    ESP_LOGI(TAG, "Report '%s' -> %s", status, err == ESP_OK ? "sent" : esp_err_to_name(err));
    esp_http_client_cleanup(client);
}

// ═══════════════════════════════════════════════════════════════
//  SELF-TEST — gates whether a freshly installed image gets confirmed or
//  rejected (doc 52 §6 / company doc §4.6). WiFi is already confirmed
//  connected by the time ota_client_init() runs; this covers the other
//  half: "backend reachable, Manifest responds" (their own wording) — a
//  200 OR a 404 both count as "reachable" (404 legitimately means
//  "reached the server, it just has no firmware configured").
// ═══════════════════════════════════════════════════════════════
static bool _self_test_backend_reachable(void) {
    char product_name[48];
    nvs_state_get_product(product_name, sizeof(product_name));

    char path[160];
    snprintf(path, sizeof(path), "%s?productName=%s", OTA_MANIFEST_PATH, product_name);
    char url[220];
    _build_server_url(url, sizeof(url), path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,
        // Required for https:// (OTA_USE_HTTPS=1, config.h doc 56/65).
        .crt_bundle_attach = OTA_CRT_BUNDLE_ATTACH,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    return err == ESP_OK && (status == 200 || status == 404);
}

// ═══════════════════════════════════════════════════════════════
//  SHA-256 + SIZE VERIFICATION (doc 52 §7 / company doc §4.5) — reads the
//  image back from the just-written flash partition and hashes it,
//  comparing against manifest.sha256 (computed server-side BEFORE any
//  network transfer). Runs after esp_ota_end() (doc 61 — which flushes
//  and validates the image format) but BEFORE esp_ota_set_boot_partition()
//  (the call that actually commits to booting this image next), so a
//  mismatch can still be rejected without ever booting into the bad image.
// ═══════════════════════════════════════════════════════════════
static bool _verify_sha256(const esp_partition_t *part, size_t image_len, const char *expected_hex) {
    if (!expected_hex || strlen(expected_hex) != 64) {
        ESP_LOGW(TAG, "  No sha256 supplied by server — skipping integrity verification");
        return true;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256, not SHA-224

    uint8_t buf[1024];
    size_t offset = 0;
    while (offset < image_len) {
        size_t chunk = (image_len - offset) > sizeof(buf) ? sizeof(buf) : (image_len - offset);
        if (esp_partition_read(part, offset, buf, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "  Readback for sha256 verification failed at offset %u", (unsigned)offset);
            mbedtls_sha256_free(&ctx);
            return false;
        }
        mbedtls_sha256_update(&ctx, buf, chunk);
        offset += chunk;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);

    bool match = strcasecmp(hex, expected_hex) == 0;
    ESP_LOGI(TAG, "  sha256 readback : %s", hex);
    ESP_LOGI(TAG, "  sha256 expected  : %s", expected_hex);
    ESP_LOGI(TAG, "  sha256 match     : %s", match ? "YES" : "NO — image will be rejected");
    return match;
}

// ═══════════════════════════════════════════════════════════════
//  MANIFEST GET, with immediate retries — added 2026-07-13 (doc 60).
//  RAM-only (no NVS) — up to OTA_MANIFEST_RETRY_COUNT immediate attempts
//  per check cycle. A clean 200 or 404 are both DEFINITIVE answers (not
//  transient failures) and return immediately without wasting a retry —
//  only genuine transport errors / unexpected status codes get retried.
// ═══════════════════════════════════════════════════════════════
static esp_err_t _manifest_get_once(const char *url, int *out_status) {
    s_resp_len = 0;
    memset(s_resp, 0, sizeof(s_resp));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .event_handler = _http_event_handler,
        // Required for https:// (OTA_USE_HTTPS=1, config.h doc 56/65).
        .crt_bundle_attach = OTA_CRT_BUNDLE_ATTACH,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        *out_status = 0;
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t _manifest_get_with_retries(const char *url, int *out_status) {
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= OTA_MANIFEST_RETRY_COUNT; attempt++) {
        err = _manifest_get_once(url, out_status);
        if (err == ESP_OK && (*out_status == 200 || *out_status == 404)) {
            if (attempt > 1) {
                ESP_LOGI(TAG, "  Manifest check succeeded on attempt %d/%d", attempt, OTA_MANIFEST_RETRY_COUNT);
            }
            return err;
        }
        bool will_retry = attempt < OTA_MANIFEST_RETRY_COUNT;
        ESP_LOGW(TAG, "  Manifest check attempt %d/%d failed: %s (status=%d)%s",
                 attempt, OTA_MANIFEST_RETRY_COUNT, esp_err_to_name(err), *out_status,
                 will_retry ? " — retrying" : " — giving up for this cycle");
        if (will_retry) vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_DELAY_MS));
    }
    return err;
}

// ═══════════════════════════════════════════════════════════════
//  DOWNLOAD ATTEMPT (one full begin/perform/verify/finish cycle), with
//  immediate retries — added 2026-07-13 (doc 60). RAM-only (no NVS) — up
//  to OTA_DOWNLOAD_RETRY_COUNT immediate attempts per check cycle, then
//  gives up for this cycle (next periodic check tries fresh, nothing is
//  ever permanently blacklisted).
// ═══════════════════════════════════════════════════════════════
// Passed through esp_http_client_config_t's .user_data so the event
// handler below can write straight into flash as chunks arrive — same
// "capture bytes during the event callback, not after perform() returns"
// principle already proven by trips_api.c (writes to a file) and this
// file's own Manifest response capture (writes to s_resp); here it's
// esp_ota_write() into flash instead.
typedef struct {
    esp_ota_handle_t ota_handle;
    size_t bytes_written;
    bool write_failed;
} _ota_write_ctx_t;

static esp_err_t _ota_download_event_handler(esp_http_client_event_t *evt) {
    _ota_write_ctx_t *ctx = (_ota_write_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 && ctx && !ctx->write_failed) {
        esp_err_t err = esp_ota_write(ctx->ota_handle, evt->data, evt->data_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  esp_ota_write failed at byte %u: %s", (unsigned)ctx->bytes_written, esp_err_to_name(err));
            ctx->write_failed = true;
            return ESP_FAIL;
        }
        ctx->bytes_written += evt->data_len;
    }
    return ESP_OK;
}

// ─── Manual download, added 2026-07-13 (doc 61) — REPLACES
// esp_https_ota_begin()/_perform()/_finish(). Root-caused via comparison:
// the Manifest/Report calls on this SAME real server, using plain
// esp_http_client_perform(), always worked; esp_https_ota's own
// higher-level API, downloading a DIFFERENT path (/Resources/...) on that
// SAME server, failed 3/3 times with "esp-tls-mbedtls: read error"
// even after doc 59's buffer/keep-alive/User-Agent fix. Since plain
// esp_http_client_perform() is proven to work against this exact host,
// this uses that same proven mechanism for the download too — streaming
// each chunk straight into the OTA partition via esp_ota_write() from the
// event handler above, instead of trusting esp_https_ota's own internal
// connection handling. esp_ota_end() is called BEFORE the SHA-256 check
// (not after) because esp_ota_write() can hold a final partial chunk in
// an internal buffer that isn't flushed to flash until esp_ota_end() —
// reading back for verification before that would risk hashing an
// incomplete image even on a perfectly good download. ──
static bool _attempt_download_once(const char *binary_url, const esp_partition_t *target_part,
                                    const char *sha256_expected, long expected_size,
                                    int firmware_id, const char *latest_version, int latest_vcode,
                                    char *fail_reason, size_t fail_reason_len) {
    esp_ota_handle_t update_handle = 0;
    esp_err_t begin_err = esp_ota_begin(target_part, expected_size >= 0 ? (size_t)expected_size : OTA_SIZE_UNKNOWN,
                                         &update_handle);
    if (begin_err != ESP_OK) {
        strlcpy(fail_reason, esp_err_to_name(begin_err), fail_reason_len);
        return false;
    }

    _ota_write_ctx_t ctx = { .ota_handle = update_handle, .bytes_written = 0, .write_failed = false };

    // Added 2026-07-13 (doc 62) — `curl -v` against this exact URL revealed
    // two things: the server is "openresty" (nginx) fronting this static-
    // file path specifically (a different stack than whatever serves
    // /api/Esp32Ota/...), and curl explicitly offers+negotiates ALPN
    // "http/1.1". ESP-IDF's esp_http_client sends NO ALPN extension at all
    // by default (.alpn_protos unset) — a real, concrete difference from
    // every client that's been proven to work against this URL. Offering
    // ALPN explicitly matches curl's own successful negotiation exactly.
    static const char *s_alpn_protos[] = { "http/1.1", NULL };

    esp_http_client_config_t http_cfg = {
        .url = binary_url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        // Same proven-working config as trips_api.c's genuine HTTPS call
        // and this file's own Manifest/Report calls (doc 59).
        .crt_bundle_attach = OTA_CRT_BUNDLE_ATTACH,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .keep_alive_enable = false,
        .user_agent = OTA_HTTP_USER_AGENT,
        .alpn_protos = s_alpn_protos,
        .event_handler = _ota_download_event_handler,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        esp_ota_abort(update_handle);
        strlcpy(fail_reason, "http_init_failed", fail_reason_len);
        return false;
    }

    esp_err_t perform_err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (perform_err != ESP_OK || status != 200 || ctx.write_failed) {
        esp_ota_abort(update_handle);
        if (ctx.write_failed) {
            strlcpy(fail_reason, "flash_write_failed", fail_reason_len);
        } else {
            snprintf(fail_reason, fail_reason_len, "%.20s(%d)", esp_err_to_name(perform_err), status);
        }
        return false;
    }

    if (expected_size >= 0 && (long)ctx.bytes_written != expected_size) {
        ESP_LOGE(TAG, "  Size mismatch: got %u bytes, manifest said %ld", (unsigned)ctx.bytes_written, expected_size);
        esp_ota_abort(update_handle);
        strlcpy(fail_reason, "size_mismatch", fail_reason_len);
        return false;
    }

    // Flushes any buffered partial chunk to flash and validates the image
    // format (app descriptor magic byte etc.) — esp_ota_end() releases
    // the handle either way, success or failure.
    esp_err_t end_err = esp_ota_end(update_handle);
    if (end_err != ESP_OK) {
        strlcpy(fail_reason, esp_err_to_name(end_err), fail_reason_len);
        return false;
    }

    if (!_verify_sha256(target_part, ctx.bytes_written, sha256_expected)) {
        // Image is already flushed+validated by esp_ota_end(), but we
        // never call esp_ota_set_boot_partition() below — the bootloader
        // has no reason to ever look at this partition, so leaving
        // mismatched bytes sitting in it is harmless.
        strlcpy(fail_reason, "sha256_mismatch", fail_reason_len);
        return false;
    }

    _report(firmware_id, latest_version, latest_vcode, "Installing", NULL);
    esp_err_t set_boot_err = esp_ota_set_boot_partition(target_part);
    if (set_boot_err != ESP_OK) {
        strlcpy(fail_reason, esp_err_to_name(set_boot_err), fail_reason_len);
        return false;
    }
    return true;
}

static bool _attempt_download_with_retries(const char *binary_url, const esp_partition_t *target_part,
                                            const char *sha256_expected, long expected_size,
                                            int firmware_id, const char *latest_version, int latest_vcode,
                                            char *fail_reason, size_t fail_reason_len) {
    for (int attempt = 1; attempt <= OTA_DOWNLOAD_RETRY_COUNT; attempt++) {
        ESP_LOGI(TAG, "  Download attempt %d/%d...", attempt, OTA_DOWNLOAD_RETRY_COUNT);
        if (_attempt_download_once(binary_url, target_part, sha256_expected, expected_size,
                                    firmware_id, latest_version, latest_vcode, fail_reason, fail_reason_len)) {
            if (attempt > 1) {
                ESP_LOGI(TAG, "  Download succeeded on attempt %d/%d", attempt, OTA_DOWNLOAD_RETRY_COUNT);
            }
            return true;
        }
        bool will_retry = attempt < OTA_DOWNLOAD_RETRY_COUNT;
        ESP_LOGW(TAG, "  Download attempt %d/%d failed: %s%s",
                 attempt, OTA_DOWNLOAD_RETRY_COUNT, fail_reason,
                 will_retry ? " — retrying" : " — giving up for this cycle");
        if (will_retry) vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_DELAY_MS));
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════
//  CHECK-IN + DOWNLOAD for ONE target — GET /api/Esp32Ota/Manifest, then
//  download if a newer version is offered. Extracted 2026-07-14 (doc 69)
//  from the old single-target ota_client_check_now() so it can run
//  against EITHER the primary target (OTA_SERVER_MODE, config.h) or the
//  local fallback target (OTA_FALLBACK_LOCAL_*, config.h) — see
//  ota_client_check_now() below, which now just calls this once or twice.
//  Returns true if this target needed no further action (no update
//  available, or update succeeded — which reboots and never returns
//  here) or false if it genuinely failed (manifest unreachable, or
//  download failed after retries) — the false case is what triggers the
//  fallback attempt.
// ═══════════════════════════════════════════════════════════════
static bool _check_and_download(const char *host, int port, bool use_https, const char *target_label) {
    char my_version[24];
    version_get_string(my_version, sizeof(my_version));
    char company_id[48], product_name[48];
    nvs_state_get_company(company_id, sizeof(company_id));
    nvs_state_get_product(product_name, sizeof(product_name));
    bool has_company = _is_all_digits(company_id);

    char path[350];
#if OTA_MANIFEST_SEND_FULL_PARAMS
    // FULL contract (doc 57) — companyId + productName + currentVersionCode
    // + currentVersion, per §2.1. Currently INACTIVE
    // (OTA_MANIFEST_SEND_FULL_PARAMS=0, config.h) — kept here, not deleted,
    // for whenever the other params are needed again.
    if (has_company) {
        snprintf(path, sizeof(path),
                 "%s?companyId=%s&productName=%s&currentVersionCode=%d&currentVersion=%s",
                 OTA_MANIFEST_PATH, company_id, product_name, version_get_code(), my_version);
    } else {
        snprintf(path, sizeof(path),
                 "%s?productName=%s&currentVersionCode=%d&currentVersion=%s",
                 OTA_MANIFEST_PATH, product_name, version_get_code(), my_version);
    }
#else
    // MINIMAL (ACTIVE, 2026-07-13+, doc 57) — companyId ONLY, e.g.
    // "?companyId=28", per explicit request while testing against the
    // real server right now. Nothing sent at all if companyId isn't
    // provisioned yet ("nvs company <id>").
    if (has_company) {
        snprintf(path, sizeof(path), "%s?companyId=%s", OTA_MANIFEST_PATH, company_id);
    } else {
        snprintf(path, sizeof(path), "%s", OTA_MANIFEST_PATH);
    }
#endif
    char url[400];
    _build_server_url_ex(url, sizeof(url), host, port, use_https, path);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "MANIFEST CHECK [%s] -> %s", target_label, url);

    int status = 0;
    esp_err_t err = _manifest_get_with_retries(url, &status); // doc 60 — up to OTA_MANIFEST_RETRY_COUNT immediate attempts, RAM-only

    // 404 = "no active firmware for this company/product" — an EXPECTED
    // state per the company doc §4.4 ("Treat HTTP 404 as 'no firmware
    // configured', not an error"), not a network/server failure.
    if (status == 404) {
        ESP_LOGI(TAG, "  No firmware configured for this company/product (404) — running %s", my_version);
        s_last_update_found = false;
        strlcpy(s_last_server_ver, my_version, sizeof(s_last_server_ver));
        s_last_firmware_id = -1;
        s_consecutive_failures = 0;
        ESP_LOGI(TAG, "══════════════════════════════════════");
        return true;
    }

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Manifest check [%s] FAILED: %s (status=%d)", target_label, esp_err_to_name(err), status);
        s_consecutive_failures++;
        return false;
    }

    ESP_LOGI(TAG, "  Response (%d bytes): %s", s_resp_len, s_resp);

    cJSON *json = cJSON_Parse(s_resp);
    if (!json) {
        ESP_LOGE(TAG, "Manifest response [%s] was not valid JSON", target_label);
        s_consecutive_failures++;
        return false;
    }
    s_consecutive_failures = 0;

    cJSON *j_avail    = cJSON_GetObjectItem(json, "updateAvailable");
    cJSON *j_latest   = cJSON_GetObjectItem(json, "latestVersion");
    cJSON *j_vcode    = cJSON_GetObjectItem(json, "latestVersionCode");
    cJSON *j_fwid     = cJSON_GetObjectItem(json, "firmwareId");
    cJSON *j_mand     = cJSON_GetObjectItem(json, "mandatory");
    cJSON *j_url      = cJSON_GetObjectItem(json, "url");
    cJSON *j_sha256   = cJSON_GetObjectItem(json, "sha256");
    cJSON *j_size     = cJSON_GetObjectItem(json, "sizeBytes");

    bool server_says_available = cJSON_IsTrue(j_avail);
    int latest_vcode = cJSON_IsNumber(j_vcode) ? j_vcode->valueint : -1;
    int firmware_id  = cJSON_IsNumber(j_fwid) ? j_fwid->valueint : -1;
    bool mandatory   = cJSON_IsTrue(j_mand);

    if (cJSON_IsString(j_latest)) strlcpy(s_last_server_ver, j_latest->valuestring, sizeof(s_last_server_ver));
    s_last_firmware_id = firmware_id;

    // "Only act when updateAvailable == true AND latestVersionCode >
    // FIRMWARE_VERSION_CODE. Re-check the comparison locally; don't trust
    // the flag alone." (company doc §4.4)
    bool locally_newer = latest_vcode > version_get_code();
    bool available = server_says_available && locally_newer && latest_vcode >= 0;
    s_last_update_found = available;

    if (!available || !cJSON_IsString(j_url)) {
        ESP_LOGI(TAG, "  No update available [%s] — running %s (code %d)", target_label, my_version, version_get_code());
        cJSON_Delete(json);
        ESP_LOGI(TAG, "══════════════════════════════════════");
        return true;
    }

    ESP_LOGI(TAG, "  Server offers versionCode %d (firmwareId %d)%s",
             latest_vcode, firmware_id, mandatory ? " [MANDATORY]" : "");

    // NOTE (doc 60): no update-loop/blacklist check here anymore — the old
    // NVS-persisted "already failed 3 times, skip until newer version"
    // behavior is gone. Every check cycle always attempts this versionCode
    // fresh (with its own OTA_DOWNLOAD_RETRY_COUNT immediate retries,
    // below) — nothing about a past attempt survives to the next cycle or
    // the next boot.

    char binary_url[300];
    strlcpy(binary_url, j_url->valuestring, sizeof(binary_url)); // absolute URL from the manifest
    _force_https_if_needed(binary_url, sizeof(binary_url), use_https); // doc 58/69 — don't trust the manifest's scheme blindly
    char sha256_expected[72] = "";
    if (cJSON_IsString(j_sha256)) strlcpy(sha256_expected, j_sha256->valuestring, sizeof(sha256_expected));
    long expected_size = cJSON_IsNumber(j_size) ? (long)j_size->valuedouble : -1;
    char latest_version[24];
    strlcpy(latest_version, s_last_server_ver, sizeof(latest_version));
    cJSON_Delete(json);

    // ── Download + verify + apply, up to OTA_DOWNLOAD_RETRY_COUNT immediate
    // attempts (doc 60) ─────────────────────────────────────────────
    ESP_LOGW(TAG, "  Newer version available [%s] — starting OTA download", target_label);
    ESP_LOGI(TAG, "  Binary URL: %s", binary_url);
    // Report POSTs below always go to the PRIMARY target's Report endpoint
    // (via _report()'s own macro-based URL), even during a fallback
    // attempt — a known, accepted simplification for this temporary
    // diagnostic (doc 69): the LOCAL fallback server's own Report endpoint
    // never sees these. Harmless — Report is telemetry, not part of the
    // download path being tested.
    _report(firmware_id, latest_version, latest_vcode, "Downloading", NULL);
    char fwid_str[16];
    snprintf(fwid_str, sizeof(fwid_str), "%d", firmware_id);
    nvs_state_set_pending(latest_vcode, fwid_str); // still used — this is what lets the post-reboot self-test know which release it's verifying

    const esp_partition_t *target_part = esp_ota_get_next_update_partition(NULL);

    char fail_reason[32] = "";
    bool ota_ok = _attempt_download_with_retries(binary_url, target_part, sha256_expected, expected_size,
                                                  firmware_id, latest_version, latest_vcode,
                                                  fail_reason, sizeof(fail_reason));

    if (ota_ok) {
        ESP_LOGW(TAG, "  OTA verified + written OK [%s] — rebooting into new firmware", target_label);
        ESP_LOGI(TAG, "══════════════════════════════════════");
        vTaskDelay(pdMS_TO_TICKS(300)); // let the report POST/log flush
        esp_restart();
        // Post-reboot self-test + "Installed"/"RolledBack" report happens
        // in ota_client_init() on the next boot — see there.
        return true; // unreachable — esp_restart() does not return
    }

    ESP_LOGE(TAG, "  OTA FAILED [%s] after %d attempts: %s — staying on current firmware",
             target_label, OTA_DOWNLOAD_RETRY_COUNT, fail_reason);
    _report(firmware_id, latest_version, latest_vcode, "Failed", fail_reason);
    nvs_state_clear_pending();
    s_consecutive_failures++;
    ESP_LOGI(TAG, "══════════════════════════════════════");
    return false;
}

// ═══════════════════════════════════════════════════════════════
//  CHECK-IN driver — tries the PRIMARY target (OTA_SERVER_MODE,
//  config.h); if that fails (manifest unreachable, or download failed
//  after retries), and OTA_FALLBACK_LOCAL_TEST_ENABLED is on (doc 69,
//  temporary diagnostic — see config.h), immediately retries the whole
//  cycle against the LOCAL fallback target too, in the SAME check-in
//  call. Set OTA_FALLBACK_LOCAL_TEST_ENABLED to 0 to go back to plain
//  single-target behavior.
// ═══════════════════════════════════════════════════════════════
void ota_client_check_now(void) {
    // Resolved via nvs_state_resolve_server() (doc 72), not the OTA_SERVER_*
    // macros directly — see _build_server_url()'s comment above for why.
    char primary_host[96];
    int primary_port;
    bool primary_use_https;
    nvs_state_resolve_server(primary_host, sizeof(primary_host), &primary_port, &primary_use_https);
    bool primary_ok = _check_and_download(primary_host, primary_port, primary_use_https, OTA_SERVER_MODE_LABEL);

#if OTA_FALLBACK_LOCAL_TEST_ENABLED
    if (!primary_ok) {
        ESP_LOGW(TAG, "Primary target [%s] failed — trying LOCAL fallback (doc 69 diagnostic)...", OTA_SERVER_MODE_LABEL);
        _check_and_download(OTA_FALLBACK_LOCAL_HOST, OTA_FALLBACK_LOCAL_PORT, OTA_FALLBACK_LOCAL_USE_HTTPS,
                             "LOCAL fallback, doc 69");
    }
#else
    (void)primary_ok;
#endif
}

// ═══════════════════════════════════════════════════════════════
//  BACKGROUND PERIODIC CHECK TASK — exponential backoff + jitter
//  (doc 52 §9 / company doc §4.4's "add jitter so a fleet doesn't
//  stampede the server").
// ═══════════════════════════════════════════════════════════════
static void _ota_periodic_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    ota_client_check_now();

    while (1) {
        int multiplier = 1;
        for (int i = 0; i < s_consecutive_failures && multiplier < OTA_CHECK_BACKOFF_MAX_MULTIPLIER; i++) {
            multiplier *= 2;
        }
        if (multiplier > OTA_CHECK_BACKOFF_MAX_MULTIPLIER) multiplier = OTA_CHECK_BACKOFF_MAX_MULTIPLIER;

        int base_s = OTA_CHECK_INTERVAL_S * multiplier;
        int jitter_range = (base_s * OTA_CHECK_JITTER_PERCENT) / 100;
        int jitter = 0;
        if (jitter_range > 0) {
            jitter = (int)(esp_random() % (uint32_t)(2 * jitter_range + 1)) - jitter_range;
        }
        int delay_s = base_s + jitter;
        if (delay_s < 1) delay_s = 1;

        ESP_LOGI(TAG, "Next Manifest check in %ds (base=%ds x%d backoff, jitter=%+ds)",
                 delay_s, OTA_CHECK_INTERVAL_S, multiplier, jitter);
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
        ota_client_check_now();
    }
}

void ota_client_init(void) {
    // If the bootloader marked this boot PENDING_VERIFY (fresh OTA image,
    // rollback armed), run a real self-test before confirming or rejecting
    // it (doc 52 §6 / company doc §4.6).
    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {

        int pend_vc = -1;
        char pend_fwid_str[16] = "";
        nvs_state_get_pending(&pend_vc, pend_fwid_str, sizeof(pend_fwid_str));
        int pend_fwid = pend_fwid_str[0] ? atoi(pend_fwid_str) : -1;

        char my_version[24];
        version_get_string(my_version, sizeof(my_version));

        ESP_LOGW(TAG, "Post-OTA boot detected (versionCode=%d, firmwareId=%d) — running self-test...",
                 pend_vc, pend_fwid);

        bool self_test_ok = _self_test_backend_reachable();

        if (self_test_ok) {
            ESP_LOGW(TAG, "Self-test PASSED — marking image valid, cancelling rollback");
            esp_ota_mark_app_valid_cancel_rollback();
            nvs_state_clear_pending();
            // Now running the new image — currentVersion/currentVersionCode
            // ARE the target, confirming the install (company doc §2.3's
            // "Installed" magic status).
            _report(pend_fwid, my_version, version_get_code(), "Installed", NULL);
        } else {
            ESP_LOGE(TAG, "Self-test FAILED (backend unreachable) — rolling back to previous partition");
            // NOTE (doc 60): no longer recorded in NVS — see the top-level
            // download-retry logic; a fresh boot after rollback starts clean.
            _report(pend_fwid, my_version, pend_vc, "RolledBack", "self_test_failed");
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_ota_mark_app_invalid_rollback_and_reboot(); // does not return on success
        }
    }

    xTaskCreate(_ota_periodic_task, "ota_periodic", 6144, NULL, 3, NULL);
    char ready_url[300];
    _build_server_url(ready_url, sizeof(ready_url), OTA_MANIFEST_PATH);
    ESP_LOGI(TAG, "OTA client READY — server=%s (%s), check every %ds (+/-%d%% jitter)",
             ready_url, OTA_SERVER_MODE_LABEL,
             OTA_CHECK_INTERVAL_S, OTA_CHECK_JITTER_PERCENT);
}

bool ota_client_process_command(const char *line) {
    while (*line == ' ') line++;
    if (strncmp(line, "ota", 3) != 0) return false;

    const char *p = line + 3;
    while (*p == ' ') p++;

    if (strncmp(p, "check", 5) == 0) {
        ESP_LOGI(TAG, "Manual OTA check requested");
        ota_client_check_now();
        return true;
    }
    if (strncmp(p, "status", 6) == 0) {
        char my_version[24];
        version_get_string(my_version, sizeof(my_version));
        ESP_LOGI(TAG, "Running: %s (code %d) | Last server latestVersion: %s (firmwareId %d) | Update found last check: %s | Consecutive failures: %d",
                 my_version, version_get_code(), s_last_server_ver, s_last_firmware_id,
                 s_last_update_found ? "YES" : "no", s_consecutive_failures);
        return true;
    }

    printf("  ota check    Check the server right now\n");
    printf("  ota status   Show last check-in result\n");
    return true;
}

#else // !ENABLE_OTA

#include "esp_log.h"
#include "ota_client.h"

void ota_client_init(void) { ESP_LOGI("ota_client", "DISABLED (ENABLE_OTA=0 in config.h)"); }
void ota_client_check_now(void) {}
bool ota_client_process_command(const char *line) { (void)line; return false; }

#endif // ENABLE_OTA
