/**
 * session_store.c — NVS-backed session/identity state
 *
 * See session_store.h for the design rationale. Every value lives in a
 * static in-RAM struct (fast reads — every fare-calc tick can safely
 * read session state) and is mirrored to NVS on every write (so it
 * survives reboot). NVS itself is the source of truth at boot; the RAM
 * struct is a cache loaded once in session_store_init().
 */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "config.h"
#include "session_store.h"

static const char *TAG = "session";
#define NVS_NAMESPACE "session"

typedef struct {
    // 1536, not 512 — a real login JWT from this server runs noticeably
    // longer than first assumed (a real login was observed with a
    // 540-byte base64 PAYLOAD segment alone, i.e. a ~650-700 char full
    // token): 512 silently truncated it via strlcpy() below, corrupting
    // the stored token, which then got sent as a broken "Authorization:
    // Bearer <truncated garbage>" header on every subsequent authenticated
    // call. api_client.c's token[]/auth_header[] must stay >= this.
    char    access_token[1536];
    char    refresh_token[160];
    int64_t access_token_expiry;    // epoch seconds
    int64_t refresh_token_expiry;   // epoch seconds

    int64_t driver_id;
    char    driver_no[32];
    char    driver_name[64];

    int64_t vehicle_id;
    int64_t vehicle_type_id;
    char    vehicle_no[32];

    int64_t network_id;
    char    company_name[64];

    char    tariff_type[32];
    duty_status_t duty_status;

    int32_t active_local_trip_id;
    int64_t active_server_job_id;
    int32_t next_local_trip_id_counter;
} session_t;

static session_t s = {0};

// ── Small NVS helpers — open, do one op, close, log clearly on failure ──
static void _nvs_set_str(const char *key, const char *value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed for '%s': %s", key, esp_err_to_name(err)); return; }
    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_str('%s') failed: %s", key, esp_err_to_name(err));
    nvs_close(h);
}

static void _nvs_get_str(const char *key, char *out, size_t out_size) {
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;   // namespace not created yet — fine, defaults apply
    size_t len = out_size;
    nvs_get_str(h, key, out, &len);   // ESP_ERR_NVS_NOT_FOUND is expected on first boot — leave out empty
    nvs_close(h);
}

static void _nvs_set_i64(const char *key, int64_t value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed for '%s': %s", key, esp_err_to_name(err)); return; }
    err = nvs_set_i64(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_i64('%s') failed: %s", key, esp_err_to_name(err));
    nvs_close(h);
}

static int64_t _nvs_get_i64(const char *key, int64_t default_value) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return default_value;
    int64_t value = default_value;
    nvs_get_i64(h, key, &value);
    nvs_close(h);
    return value;
}

static void _nvs_set_i32(const char *key, int32_t value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed for '%s': %s", key, esp_err_to_name(err)); return; }
    err = nvs_set_i32(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_i32('%s') failed: %s", key, esp_err_to_name(err));
    nvs_close(h);
}

static int32_t _nvs_get_i32(const char *key, int32_t default_value) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return default_value;
    int32_t value = default_value;
    nvs_get_i32(h, key, &value);
    nvs_close(h);
    return value;
}

// ═══════════════════════════════════════════════════════════════
//  INIT — load everything from NVS into the RAM cache
// ═══════════════════════════════════════════════════════════════
esp_err_t session_store_init(void) {
    memset(&s, 0, sizeof(s));

    _nvs_get_str("access_token",  s.access_token,  sizeof(s.access_token));
    _nvs_get_str("refresh_token", s.refresh_token, sizeof(s.refresh_token));
    s.access_token_expiry  = _nvs_get_i64("at_expiry", 0);
    s.refresh_token_expiry = _nvs_get_i64("rt_expiry", 0);

    s.driver_id = _nvs_get_i64("driver_id", 0);
    _nvs_get_str("driver_no",   s.driver_no,   sizeof(s.driver_no));
    _nvs_get_str("driver_name", s.driver_name, sizeof(s.driver_name));

    s.vehicle_id      = _nvs_get_i64("vehicle_id", 0);
    s.vehicle_type_id = _nvs_get_i64("veh_type_id", 0);
    _nvs_get_str("vehicle_no", s.vehicle_no, sizeof(s.vehicle_no));

    s.network_id = _nvs_get_i64("network_id", 0);
    _nvs_get_str("company_name", s.company_name, sizeof(s.company_name));

    _nvs_get_str("tariff_type", s.tariff_type, sizeof(s.tariff_type));
    s.duty_status = (duty_status_t)_nvs_get_i32("duty_status", DUTY_STATUS_OFF_DUTY);

    s.active_local_trip_id       = _nvs_get_i32("trip_local_id", 0);
    s.active_server_job_id       = _nvs_get_i64("trip_job_id", 0);
    s.next_local_trip_id_counter = _nvs_get_i32("next_trip_id", 1);   // start at 1, never 0 (0 means "no trip")

    // First-boot provisioning — only seed if NVS genuinely has nothing yet,
    // never overwrite a value a previous session already stored ("setup"
    // serial command can still override at runtime for testing).
    if (s.vehicle_no[0] == '\0') {
        strlcpy(s.vehicle_no, SETUP_VEHICLE_NO, sizeof(s.vehicle_no));
        _nvs_set_str("vehicle_no", s.vehicle_no);
        ESP_LOGI(TAG, "First boot — seeded vehicle_no from config.h: %s", s.vehicle_no);
    }

    ESP_LOGI(TAG, "Session store loaded from NVS (namespace '%s')", NVS_NAMESPACE);
    return ESP_OK;
}

// ── Auth/token state ──────────────────────────────────────────
void session_store_set_tokens(const char *access_token, const char *refresh_token, int64_t refresh_token_expiry_epoch) {
    if (access_token)  { strlcpy(s.access_token, access_token, sizeof(s.access_token));  _nvs_set_str("access_token", s.access_token); }
    if (refresh_token) { strlcpy(s.refresh_token, refresh_token, sizeof(s.refresh_token)); _nvs_set_str("refresh_token", s.refresh_token); }
    s.refresh_token_expiry = refresh_token_expiry_epoch;
    _nvs_set_i64("rt_expiry", s.refresh_token_expiry);
}

bool session_store_get_access_token(char *out, size_t out_size) {
    strlcpy(out, s.access_token, out_size);
    return s.access_token[0] != '\0';
}

bool session_store_get_refresh_token(char *out, size_t out_size) {
    strlcpy(out, s.refresh_token, out_size);
    return s.refresh_token[0] != '\0';
}

int64_t session_store_get_access_token_expiry(void) { return s.access_token_expiry; }

void session_store_set_access_token_expiry(int64_t epoch_seconds) {
    s.access_token_expiry = epoch_seconds;
    _nvs_set_i64("at_expiry", epoch_seconds);
}

bool session_store_is_access_token_valid(void) {
    if (s.access_token[0] == '\0' || s.access_token_expiry <= 0) return false;
    time_t now = time(NULL);
    return (int64_t)now < s.access_token_expiry;
}

// ── Identity state ─────────────────────────────────────────────
void session_store_set_driver(int64_t driver_id, const char *driver_no, const char *driver_name) {
    s.driver_id = driver_id;
    _nvs_set_i64("driver_id", driver_id);
    if (driver_no)   { strlcpy(s.driver_no, driver_no, sizeof(s.driver_no));     _nvs_set_str("driver_no", s.driver_no); }
    if (driver_name) { strlcpy(s.driver_name, driver_name, sizeof(s.driver_name)); _nvs_set_str("driver_name", s.driver_name); }
}

void session_store_set_vehicle(int64_t vehicle_id, int64_t vehicle_type_id, const char *vehicle_no) {
    s.vehicle_id = vehicle_id;
    _nvs_set_i64("vehicle_id", vehicle_id);
    s.vehicle_type_id = vehicle_type_id;
    _nvs_set_i64("veh_type_id", vehicle_type_id);
    if (vehicle_no) { strlcpy(s.vehicle_no, vehicle_no, sizeof(s.vehicle_no)); _nvs_set_str("vehicle_no", s.vehicle_no); }
}

void session_store_set_network(int64_t network_id, const char *company_name) {
    s.network_id = network_id;
    _nvs_set_i64("network_id", network_id);
    if (company_name) { strlcpy(s.company_name, company_name, sizeof(s.company_name)); _nvs_set_str("company_name", s.company_name); }
}

void session_store_set_tariff_type(const char *tariff_type) {
    if (!tariff_type) return;
    strlcpy(s.tariff_type, tariff_type, sizeof(s.tariff_type));
    _nvs_set_str("tariff_type", s.tariff_type);
}

int64_t session_store_get_driver_id(void)       { return s.driver_id; }
int64_t session_store_get_vehicle_id(void)      { return s.vehicle_id; }
int64_t session_store_get_vehicle_type_id(void) { return s.vehicle_type_id; }
int64_t session_store_get_network_id(void)      { return s.network_id; }

bool session_store_get_tariff_type(char *out, size_t out_size) {
    strlcpy(out, s.tariff_type, out_size);
    return s.tariff_type[0] != '\0';
}

bool session_store_get_vehicle_no(char *out, size_t out_size) {
    strlcpy(out, s.vehicle_no, out_size);
    return s.vehicle_no[0] != '\0';
}

// ── Duty status ─────────────────────────────────────────────────
void session_store_set_duty_status(duty_status_t status) {
    s.duty_status = status;
    _nvs_set_i32("duty_status", (int32_t)status);
}

duty_status_t session_store_get_duty_status(void) { return s.duty_status; }

// ── Active trip ─────────────────────────────────────────────────
void session_store_set_active_trip(int32_t local_trip_id, int64_t server_job_id) {
    s.active_local_trip_id = local_trip_id;
    _nvs_set_i32("trip_local_id", local_trip_id);
    s.active_server_job_id = server_job_id;
    _nvs_set_i64("trip_job_id", server_job_id);
}

void session_store_clear_active_trip(void) {
    session_store_set_active_trip(0, 0);
}

int32_t session_store_get_active_local_trip_id(void) { return s.active_local_trip_id; }
int64_t session_store_get_active_server_job_id(void)  { return s.active_server_job_id; }

int32_t session_store_next_local_trip_id(void) {
    int32_t id = s.next_local_trip_id_counter;
    s.next_local_trip_id_counter++;
    _nvs_set_i32("next_trip_id", s.next_local_trip_id_counter);
    return id;
}

// ── Clear (logout) ────────────────────────────────────────────
void session_store_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Session cleared (logout) — all fields wiped");
    session_store_init();   // reload defaults + re-seed provisioning constants
}

// ═══════════════════════════════════════════════════════════════
//  DIAGNOSTICS — "what's actually stored"
// ═══════════════════════════════════════════════════════════════
void session_store_print(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "SESSION STORE (NVS namespace '%s')", NVS_NAMESPACE);
    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  Access token:     %s (%d chars)%s", s.access_token[0] ? "SET" : "(empty)",
             (int)strlen(s.access_token), session_store_is_access_token_valid() ? " — VALID" : " — EXPIRED/UNKNOWN");
    ESP_LOGI(TAG, "  Refresh token:    %s (%d chars)", s.refresh_token[0] ? "SET" : "(empty)", (int)strlen(s.refresh_token));
    ESP_LOGI(TAG, "  Access expiry:    %lld (epoch)", (long long)s.access_token_expiry);
    ESP_LOGI(TAG, "  Refresh expiry:   %lld (epoch)", (long long)s.refresh_token_expiry);
    ESP_LOGI(TAG, "  Driver:           id=%lld no=%s name=%s", (long long)s.driver_id, s.driver_no, s.driver_name);
    ESP_LOGI(TAG, "  Vehicle:          id=%lld type_id=%lld no=%s", (long long)s.vehicle_id, (long long)s.vehicle_type_id, s.vehicle_no);
    ESP_LOGI(TAG, "  Network:          id=%lld company=%s", (long long)s.network_id, s.company_name);
    ESP_LOGI(TAG, "  Tariff type:      %s", s.tariff_type[0] ? s.tariff_type : "(not set)");
    ESP_LOGI(TAG, "  Duty status:      %s", s.duty_status == DUTY_STATUS_ON_DUTY ? "ON DUTY" : "OFF DUTY");
    ESP_LOGI(TAG, "  Active trip:      local_id=%ld server_job_id=%lld%s",
             (long)s.active_local_trip_id, (long long)s.active_server_job_id,
             s.active_local_trip_id > 0 ? (s.active_server_job_id > 0 ? " (synced)" : " (NOT YET SYNCED)") : "");
    ESP_LOGI(TAG, "  Next local trip#: %ld", (long)s.next_local_trip_id_counter);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

bool session_store_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "session", 7) != 0) return false;

    const char *p = line + 7;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "info") == 0) {
        session_store_print();
    } else if (strcmp(p, "clear") == 0) {
        session_store_clear();
    } else {
        printf("Unknown 'session' subcommand. Try: session info | session clear\n");
    }
    return true;
}
