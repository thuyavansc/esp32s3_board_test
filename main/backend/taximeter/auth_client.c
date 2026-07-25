/**
 * auth_client.c — Login, logout, token expiry
 *
 * See auth_client.h for the full design. Two small self-contained
 * helpers live here because nothing else in the project needs them yet:
 *   _jwt_get_expiry()      — decode a JWT's "exp" claim
 *   _parse_iso8601_epoch() — turn "2026-07-08T12:09:30Z" into epoch secs
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "config.h"
#include "api_client.h"
#include "session_store.h"
#include "auth_client.h"
#include "bg_worker.h"

static const char *TAG = "auth";

// ── Device MAC address, formatted like Android's DeviceDetails.DEVICE_ID ──
static void _get_mac_address(char *out, size_t out_size) {
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ── JWT "exp" claim decode — no dedicated JWT library needed ──
static bool _jwt_get_expiry(const char *jwt, int64_t *out_expiry) {
    *out_expiry = 0;
    if (!jwt) return false;

    const char *p1 = strchr(jwt, '.');
    const char *p2 = p1 ? strchr(p1 + 1, '.') : NULL;
    if (!p1 || !p2) {
        ESP_LOGE(TAG, "  JWT decode: not a valid header.payload.signature token");
        return false;
    }

    // 1536, not a smaller cap — a real login was observed with a
    // 540-byte payload segment alone (this server's JWTs embed more
    // claims than first assumed). Bounds check below is relative to
    // sizeof(b64), not a hardcoded number, so this can't drift out of
    // sync again.
    size_t payload_len = (size_t)(p2 - (p1 + 1));
    char b64[1536];
    if (payload_len == 0 || payload_len >= sizeof(b64)) {
        ESP_LOGE(TAG, "  JWT decode: payload segment size (%u) out of expected range", (unsigned)payload_len);
        return false;
    }

    memcpy(b64, p1 + 1, payload_len);
    b64[payload_len] = '\0';
    // base64url -> base64: '-'->'+', '_'->'/'  (JWT uses the URL-safe alphabet, no padding)
    for (char *c = b64; *c; c++) {
        if (*c == '-') *c = '+';
        else if (*c == '_') *c = '/';
    }

    unsigned char decoded[1152];   // ~3/4 of sizeof(b64) — matches base64's expansion ratio, with headroom
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, (const unsigned char *)b64, strlen(b64));
    if (ret != 0) {
        ESP_LOGE(TAG, "  JWT decode: mbedtls_base64_decode failed (err=-0x%04x)", -ret);
        return false;
    }
    decoded[decoded_len] = '\0';

    cJSON *json = cJSON_ParseWithLength((const char *)decoded, decoded_len);
    if (!json) {
        ESP_LOGE(TAG, "  JWT decode: payload is not valid JSON");
        return false;
    }
    cJSON *exp = cJSON_GetObjectItemCaseSensitive(json, "exp");
    bool ok = cJSON_IsNumber(exp);
    if (ok) *out_expiry = (int64_t)exp->valuedouble;
    cJSON_Delete(json);

    if (!ok) ESP_LOGE(TAG, "  JWT decode: no numeric 'exp' claim in payload");
    return ok;
}

// ── ISO 8601 → epoch seconds (assumes UTC — this project never sets a
//    local TZ, so mktime() treats the fields as UTC directly; matches
//    the NTP-synced clock everywhere else in this codebase) ──
static bool _parse_iso8601_epoch(const char *iso, int64_t *out_epoch) {
    *out_epoch = 0;
    if (!iso) return false;

    struct tm tm = {0};
    int matched = sscanf(iso, "%d-%d-%dT%d:%d:%d",
                          &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (matched != 6) {
        ESP_LOGE(TAG, "  ISO8601 parse failed for '%s' (matched %d/6 fields)", iso, matched);
        return false;
    }
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;

    time_t epoch = mktime(&tm);
    if (epoch < 0) {
        ESP_LOGE(TAG, "  ISO8601 parse: mktime() rejected '%s'", iso);
        return false;
    }
    *out_epoch = (int64_t)epoch;
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  LOGIN — POST taxis-api/api/Authentications/Login
// ═══════════════════════════════════════════════════════════════
esp_err_t auth_client_login(const char *username, const char *password) {
    if (!username || !password || username[0] == '\0' || password[0] == '\0') {
        ESP_LOGE(TAG, "login: username/password cannot be empty");
        return ESP_ERR_INVALID_ARG;
    }

    char mac[18];
    _get_mac_address(mac, sizeof(mac));

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "username", username);
    cJSON_AddStringToObject(body, "password", password);
    cJSON_AddStringToObject(body, "macAddress", mac);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) {
        ESP_LOGE(TAG, "login: failed to build request JSON (out of memory?)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "LOGIN — user=%s mac=%s", username, mac);

    // Heap-allocated, NOT static — a static buffer here would permanently
    // reserve API_SMALL_BUFFER_SIZE bytes of DRAM for the program's whole
    // lifetime even when no login is in flight.
    char *resp = malloc(API_SMALL_BUFFER_SIZE);
    if (!resp) {
        ESP_LOGE(TAG, "LOGIN: malloc(%d) for response buffer FAILED — out of heap right now.", API_SMALL_BUFFER_SIZE);
        ESP_LOGE(TAG, "  This is a RAM issue, not a network issue — see 'mem' command for current heap state.");
        free(body_str);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = api_client_request(API_METHOD_POST, EP_LOGIN, body_str, false, resp, API_SMALL_BUFFER_SIZE, &status);
    free(body_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LOGIN failed — no response from server (network/TLS issue, see above)");
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return err;
    }

    cJSON *json = cJSON_Parse(resp);
    if (!json) {
        ESP_LOGE(TAG, "LOGIN: response is not valid JSON (HTTP %d) — raw: %.200s", status, resp);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    cJSON *success = cJSON_GetObjectItemCaseSensitive(json, "success");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(json, "message");
    cJSON *data    = cJSON_GetObjectItemCaseSensitive(json, "data");

    if (!cJSON_IsTrue(success) || !cJSON_IsObject(data)) {
        ESP_LOGE(TAG, "LOGIN FAILED (HTTP %d): %s", status,
                 cJSON_IsString(message) ? message->valuestring : "(no message — check raw response)");
        cJSON_Delete(json);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    cJSON *token          = cJSON_GetObjectItemCaseSensitive(data, "token");
    cJSON *refresh_token   = cJSON_GetObjectItemCaseSensitive(data, "refreshToken");
    cJSON *refresh_expiry  = cJSON_GetObjectItemCaseSensitive(data, "refreshTokenExpiry");
    cJSON *driver_id       = cJSON_GetObjectItemCaseSensitive(data, "driverId");
    cJSON *vehicle_id      = cJSON_GetObjectItemCaseSensitive(data, "vehicleId");
    cJSON *vehicle_type_id = cJSON_GetObjectItemCaseSensitive(data, "vehicleTypeId");
    cJSON *vehicle_no      = cJSON_GetObjectItemCaseSensitive(data, "vehicleNo");
    cJSON *driver_no       = cJSON_GetObjectItemCaseSensitive(data, "driverNo");

    if (!cJSON_IsString(token) || !cJSON_IsString(refresh_token)) {
        ESP_LOGE(TAG, "LOGIN: response missing token/refreshToken fields — server response shape may");
        ESP_LOGE(TAG, "  have changed. Raw (first 300 chars): %.300s", resp);
        cJSON_Delete(json);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    int64_t refresh_expiry_epoch = 0;
    if (cJSON_IsString(refresh_expiry)) _parse_iso8601_epoch(refresh_expiry->valuestring, &refresh_expiry_epoch);

    session_store_set_tokens(token->valuestring, refresh_token->valuestring, refresh_expiry_epoch);

    int64_t access_expiry = 0;
    if (_jwt_get_expiry(token->valuestring, &access_expiry)) {
        session_store_set_access_token_expiry(access_expiry);
    } else {
        ESP_LOGW(TAG, "  Could not decode access-token expiry from JWT — token will be treated as expired");
        ESP_LOGW(TAG, "  until a fresh login; API calls may fail with 401 until then.");
    }

    session_store_set_driver(cJSON_IsNumber(driver_id) ? (int64_t)driver_id->valuedouble : 0,
                              cJSON_IsString(driver_no) ? driver_no->valuestring : NULL, NULL);

    // Only overwrite vehicle_id/vehicle_type_id if THIS response actually
    // provides valid (>0) ones — some accounts' Login response comes back
    // with vehicle_id=0/vehicle_type_id=0 (not every deployment ties a
    // vehicle to the login call itself), and blindly writing those zeros
    // would CLOBBER whatever 'setup vehicle' had already correctly
    // resolved moments earlier.
    bool has_valid_vehicle = cJSON_IsNumber(vehicle_id) && vehicle_id->valuedouble > 0
                           && cJSON_IsNumber(vehicle_type_id) && vehicle_type_id->valuedouble > 0;
    if (has_valid_vehicle) {
        session_store_set_vehicle((int64_t)vehicle_id->valuedouble, (int64_t)vehicle_type_id->valuedouble,
                                   cJSON_IsString(vehicle_no) ? vehicle_no->valuestring : NULL);
    } else {
        ESP_LOGI(TAG, "  Login response has no vehicle info — keeping vehicle_id=%lld vehicle_type_id=%lld",
                 (long long)session_store_get_vehicle_id(), (long long)session_store_get_vehicle_type_id());
    }

    ESP_LOGI(TAG, "LOGIN OK ✓ — driver_id=%lld vehicle_id=%lld vehicle_type_id=%lld",
             (long long)session_store_get_driver_id(), (long long)session_store_get_vehicle_id(),
             (long long)session_store_get_vehicle_type_id());
    cJSON_Delete(json);
    free(resp);

    // Second call, right after login — fetch the driver profile for the
    // display name. Non-fatal if it fails: login itself already
    // succeeded, we just won't have a friendly name to show yet.
    char *driver_resp = malloc(API_SMALL_BUFFER_SIZE);
    if (!driver_resp) {
        ESP_LOGW(TAG, "  Driver profile fetch skipped — malloc failed (out of heap right now, see 'mem')");
    } else {
        int driver_status = 0;
        if (api_client_request(API_METHOD_GET, EP_DRIVER, NULL, true, driver_resp, API_SMALL_BUFFER_SIZE, &driver_status) == ESP_OK) {
            cJSON *djson = cJSON_Parse(driver_resp);
            cJSON *ddata = djson ? cJSON_GetObjectItemCaseSensitive(djson, "data") : NULL;
            cJSON *profile = cJSON_IsObject(ddata) ? cJSON_GetObjectItemCaseSensitive(ddata, "profile") : NULL;
            if (cJSON_IsObject(profile)) {
                cJSON *first  = cJSON_GetObjectItemCaseSensitive(profile, "firstName");
                cJSON *last   = cJSON_GetObjectItemCaseSensitive(profile, "lastName");
                char full_name[64] = {0};
                snprintf(full_name, sizeof(full_name), "%s %s",
                         cJSON_IsString(first) ? first->valuestring : "",
                         cJSON_IsString(last)  ? last->valuestring  : "");
                session_store_set_driver(session_store_get_driver_id(), NULL, full_name);
                ESP_LOGI(TAG, "  Driver profile: %s", full_name);
            } else {
                ESP_LOGW(TAG, "  Driver profile fetch returned HTTP %d but no usable 'profile' object", driver_status);
            }
            if (djson) cJSON_Delete(djson);
        } else {
            ESP_LOGW(TAG, "  Driver profile fetch failed — continuing without a display name");
        }
        free(driver_resp);
    }

    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  LOGOUT — POST taxis-api/api/Authentications/Logout
// ═══════════════════════════════════════════════════════════════
esp_err_t auth_client_logout(void) {
    char refresh_token[160];
    session_store_get_refresh_token(refresh_token, sizeof(refresh_token));
    if (refresh_token[0] == '\0') {
        ESP_LOGW(TAG, "logout: no refresh token stored — clearing local session only");
        session_store_clear();
        return ESP_OK;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "userName", "");   // username isn't cached separately today — server accepts token-only in practice
    cJSON_AddStringToObject(body, "token", refresh_token);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    // Heap-allocated, not static/stack — same reasoning as auth_client_login()
    // above (and this response is also too big to safely put on a small
    // task's stack, e.g. the "serial_cmd" task).
    int status = 0;
    esp_err_t err = ESP_FAIL;
    if (body_str) {
        char *resp = malloc(API_SMALL_BUFFER_SIZE);
        if (resp) {
            err = api_client_request(API_METHOD_POST, EP_LOGOUT, body_str, true, resp, API_SMALL_BUFFER_SIZE, &status);
            free(resp);
        } else {
            ESP_LOGW(TAG, "logout: malloc failed for response buffer — proceeding without server confirmation");
        }
        free(body_str);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "logout: server call failed — clearing local session anyway (matches the reference");
        ESP_LOGW(TAG, "  app's own behavior of not blocking logout on a network failure)");
    } else {
        ESP_LOGI(TAG, "LOGOUT OK (HTTP %d)", status);
    }

    session_store_clear();
    return ESP_OK;
}

bool auth_client_is_logged_in(void) {
    return session_store_is_access_token_valid();
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
//
//  "login"/"logout" run their HTTPS/TLS work on bg_worker's persistent
//  8KB-stack task instead of directly on whatever task called
//  auth_client_process_command() — when triggered by a typed/GUI serial
//  command, that's the "serial_cmd" task, and mbedTLS's handshake call
//  depth overflows a small stack. Fire-and-forget, same pattern
//  duty_client.c already uses for OnDuty/OffDuty — results are logged
//  by auth_client_login()/auth_client_logout() themselves as soon as
//  the job runs, just asynchronously relative to the command line that
//  queued it.
// ═══════════════════════════════════════════════════════════════
static void _show_help(void) {
    printf("\n  auth login [user] [pass]   Login (defaults to config.h test credentials)\n");
    printf("  auth logout                Logout, clear session\n");
    printf("  auth info                  Show token validity/expiry\n");
    printf("  auth help                  Show this help\n\n");
}

typedef struct {
    char user[64];
    char pass[64];
} _login_job_arg_t;

static bool _login_job(void *arg) {
    _login_job_arg_t *a = (_login_job_arg_t *)arg;
    esp_err_t err = auth_client_login(a->user, a->pass);
    free(a);
    return err == ESP_OK;
}

static bool _logout_job(void *arg) {
    (void)arg;
    return auth_client_logout() == ESP_OK;
}

bool auth_client_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "auth", 4) != 0) return false;

    const char *p = line + 4;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) {
        _show_help();
    } else if (strncmp(p, "login", 5) == 0) {
        _login_job_arg_t *a = malloc(sizeof(*a));
        if (!a) {
            ESP_LOGE(TAG, "auth login: malloc failed — out of heap right now (see 'mem')");
        } else {
            strlcpy(a->user, AUTH_TEST_USERNAME, sizeof(a->user));
            strlcpy(a->pass, AUTH_TEST_PASSWORD, sizeof(a->pass));
            sscanf(p + 5, "%63s %63s", a->user, a->pass);   // optional overrides; leaves defaults if absent
            if (!bg_worker_submit_fn(_login_job, a, NULL, NULL)) {
                ESP_LOGW(TAG, "auth login: background worker busy — try again shortly");
                free(a);
            } else {
                ESP_LOGI(TAG, "auth login: queued on background worker — watch below for LOGIN OK/FAILED");
            }
        }
    } else if (strcmp(p, "logout") == 0) {
        if (!bg_worker_submit_fn(_logout_job, NULL, NULL, NULL)) {
            ESP_LOGW(TAG, "auth logout: background worker busy — try again shortly");
        } else {
            ESP_LOGI(TAG, "auth logout: queued on background worker");
        }
    } else if (strcmp(p, "info") == 0) {
        ESP_LOGI(TAG, "Logged in: %s | Access token valid: %s",
                 auth_client_is_logged_in() ? "yes" : "no",
                 session_store_is_access_token_valid() ? "yes" : "no");
        session_store_print();
    } else {
        printf("Unknown 'auth' subcommand. Type 'auth help'.\n");
    }
    return true;
}
