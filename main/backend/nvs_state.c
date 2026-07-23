#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "config.h"
#include "nvs_state.h"

static const char *TAG = "nvs_state";
static const char *NS  = "ota_state";

// NVS key names — kept <=15 chars (NVS limit).
#define K_COMPANY     "company_id"
#define K_PRODUCT     "product_name"
#define K_PEND_FLAG   "pend_flag"
#define K_PEND_VCODE  "pend_vcode"
#define K_PEND_FWID   "pend_fwid"
#define K_LAST_FAIL   "last_fail_vc"
#define K_FAIL_COUNT  "fail_count"
#define K_TAXI_NUM    "taxi_number"
#define K_SMS_NUM     "sms_number"
#define K_EMERG_NUM   "emerg_number"
#define K_SERVER_URL  "server_url"

void nvs_state_init(void) {
    // Nothing to pre-create — nvs_open(..., NVS_READWRITE, ...) creates the
    // namespace on first use. This just confirms it's reachable at boot so
    // a provisioning problem shows up immediately instead of silently on
    // the first OTA attempt.
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open('%s') FAILED: %s", NS, esp_err_to_name(err));
        return;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "OTA state namespace '%s' ready", NS);
}

// ── small helpers — open/read-or-default/close, open/write/commit/close ──
static void _get_str_or_default(const char *key, const char *def, char *out, size_t out_len) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        snprintf(out, out_len, "%s", def);
        return;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        snprintf(out, out_len, "%s", def);
    }
}

static void _set_str(const char *key, const char *val) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static uint32_t _get_u32_or_default(const char *key, uint32_t def) {
    nvs_handle_t h;
    uint32_t val = def;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return def;
    if (nvs_get_u32(h, key, &val) != ESP_OK) val = def;
    nvs_close(h);
    return val;
}

static void _set_u32(const char *key, uint32_t val) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t _get_u8_or_default(const char *key, uint8_t def) {
    nvs_handle_t h;
    uint8_t val = def;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return def;
    if (nvs_get_u8(h, key, &val) != ESP_OK) val = def;
    nvs_close(h);
    return val;
}

static void _set_u8(const char *key, uint8_t val) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

// ═══════════════════════════════════════════════════════════════
//  Provisioned identity
// ═══════════════════════════════════════════════════════════════
void nvs_state_get_company(char *out, size_t out_len) {
    _get_str_or_default(K_COMPANY, OTA_DEFAULT_COMPANY_ID, out, out_len);
}
void nvs_state_set_company(const char *company_id) {
    _set_str(K_COMPANY, company_id);
    ESP_LOGI(TAG, "companyId set to '%s'", company_id);
}
void nvs_state_get_product(char *out, size_t out_len) {
    _get_str_or_default(K_PRODUCT, OTA_DEFAULT_PRODUCT_NAME, out, out_len);
}
void nvs_state_set_product(const char *product_name) {
    _set_str(K_PRODUCT, product_name);
    ESP_LOGI(TAG, "productName set to '%s'", product_name);
}

// ═══════════════════════════════════════════════════════════════
//  Remote configuration (added 2026-07-14)
// ═══════════════════════════════════════════════════════════════
void nvs_state_get_taxi_number(char *out, size_t out_len) {
    _get_str_or_default(K_TAXI_NUM, REMOTE_CONFIG_DEFAULT_TAXI_NUMBER, out, out_len);
}
void nvs_state_set_taxi_number(const char *v) {
    _set_str(K_TAXI_NUM, v);
    ESP_LOGI(TAG, "taxiNumber set to '%s'", v);
}
void nvs_state_get_sms_number(char *out, size_t out_len) {
    _get_str_or_default(K_SMS_NUM, REMOTE_CONFIG_DEFAULT_SMS_NUMBER, out, out_len);
}
void nvs_state_set_sms_number(const char *v) {
    _set_str(K_SMS_NUM, v);
    ESP_LOGI(TAG, "smsNumber set to '%s'", v);
}
void nvs_state_get_emergency_number(char *out, size_t out_len) {
    _get_str_or_default(K_EMERG_NUM, REMOTE_CONFIG_DEFAULT_EMERGENCY_NUMBER, out, out_len);
}
void nvs_state_set_emergency_number(const char *v) {
    _set_str(K_EMERG_NUM, v);
    ESP_LOGI(TAG, "emergencyContactNumber set to '%s'", v);
}
void nvs_state_get_server_url(char *out, size_t out_len) {
    _get_str_or_default(K_SERVER_URL, "", out, out_len); // "" = never set, caller falls back to compiled OTA_SERVER_HOST
}
void nvs_state_set_server_url(const char *v) {
    _set_str(K_SERVER_URL, v);
    ESP_LOGI(TAG, "serverUrl set to '%s'", v);
}
void nvs_state_clear_server_url(void) {
    _set_str(K_SERVER_URL, "");
    ESP_LOGW(TAG, "serverUrl cleared — falling back to compiled default");
}

void nvs_state_resolve_server(char *host_out, size_t host_out_len, int *port_out, bool *use_https_out) {
#if ENABLE_SERVER_URL_OVERRIDE
    char override_url[128];
    nvs_state_get_server_url(override_url, sizeof(override_url));
    if (override_url[0]) {
        // Parse "https://host[:port]" or "http://host[:port]" — deliberately
        // simple (no path/query support expected or needed here; serverUrl
        // is just a base). Falls through to the compiled default below if
        // the stored value doesn't start with a recognized scheme.
        bool https = strncmp(override_url, "https://", 8) == 0;
        bool http  = !https && strncmp(override_url, "http://", 7) == 0;
        if (https || http) {
            const char *after_scheme = override_url + (https ? 8 : 7);
            char host_buf[96] = "";
            int port = https ? 443 : 80;
            const char *colon = strchr(after_scheme, ':');
            size_t host_len = colon ? (size_t)(colon - after_scheme) : strlen(after_scheme);
            if (host_len >= sizeof(host_buf)) host_len = sizeof(host_buf) - 1;
            memcpy(host_buf, after_scheme, host_len);
            host_buf[host_len] = '\0';
            if (colon) port = atoi(colon + 1);
            if (host_buf[0]) {
                snprintf(host_out, host_out_len, "%s", host_buf);
                *port_out = port;
                *use_https_out = https;
                ESP_LOGI(TAG, "Using remote-config serverUrl override: %s://%s:%d", https ? "https" : "http", host_buf, port);
                return;
            }
        }
        ESP_LOGW(TAG, "serverUrl override '%s' not a recognized http(s):// URL — using compiled default", override_url);
    }
#endif
    snprintf(host_out, host_out_len, "%s", OTA_SERVER_HOST);
    *port_out = OTA_SERVER_PORT;
    *use_https_out = OTA_USE_HTTPS;
}

// ═══════════════════════════════════════════════════════════════
//  Update-in-progress tracking
// ═══════════════════════════════════════════════════════════════
void nvs_state_set_pending(int version_code, const char *firmware_id) {
    _set_u32(K_PEND_VCODE, (uint32_t)version_code);
    _set_str(K_PEND_FWID, firmware_id ? firmware_id : "");
    _set_u8(K_PEND_FLAG, 1);
    ESP_LOGI(TAG, "Pending update recorded: versionCode=%d firmwareId=%s", version_code, firmware_id);
}

bool nvs_state_get_pending(int *version_code_out, char *firmware_id_out, size_t firmware_id_len) {
    if (_get_u8_or_default(K_PEND_FLAG, 0) != 1) return false;
    if (version_code_out) *version_code_out = (int)_get_u32_or_default(K_PEND_VCODE, 0);
    if (firmware_id_out) _get_str_or_default(K_PEND_FWID, "", firmware_id_out, firmware_id_len);
    return true;
}

void nvs_state_clear_pending(void) {
    _set_u8(K_PEND_FLAG, 0);
}

// ═══════════════════════════════════════════════════════════════
//  Update-loop protection
// ═══════════════════════════════════════════════════════════════
void nvs_state_record_failure(int version_code) {
    uint32_t last_fail = _get_u32_or_default(K_LAST_FAIL, 0);
    uint8_t count = _get_u8_or_default(K_FAIL_COUNT, 0);

    if ((uint32_t)version_code == last_fail) {
        count = (count < 255) ? count + 1 : count;
    } else {
        // A different (usually higher) versionCode failing resets the
        // streak — we only care about REPEATED failures of the SAME code.
        last_fail = (uint32_t)version_code;
        count = 1;
    }
    _set_u32(K_LAST_FAIL, last_fail);
    _set_u8(K_FAIL_COUNT, count);
    ESP_LOGW(TAG, "Recorded failure for versionCode=%d (streak=%d)", version_code, count);
}

void nvs_state_clear_failure(void) {
    _set_u8(K_FAIL_COUNT, 0);
}

bool nvs_state_should_skip(int version_code, int max_retries) {
    uint32_t last_fail = _get_u32_or_default(K_LAST_FAIL, 0);
    uint8_t count = _get_u8_or_default(K_FAIL_COUNT, 0);
    return (uint32_t)version_code == last_fail && count >= max_retries;
}

// ═══════════════════════════════════════════════════════════════
//  Serial commands — "nvs status" | "nvs company <id>" | "nvs product <name>"
// ═══════════════════════════════════════════════════════════════
bool nvs_state_process_command(const char *line) {
    while (*line == ' ') line++;
    if (strncmp(line, "nvs", 3) != 0) return false;

    const char *p = line + 3;
    while (*p == ' ') p++;

    if (strncmp(p, "status", 6) == 0) {
        char company[48], product[48], pend_fwid[64];
        char taxi_num[32], sms_num[32], emerg_num[32], server_url[128];
        nvs_state_get_company(company, sizeof(company));
        nvs_state_get_product(product, sizeof(product));
        nvs_state_get_taxi_number(taxi_num, sizeof(taxi_num));
        nvs_state_get_sms_number(sms_num, sizeof(sms_num));
        nvs_state_get_emergency_number(emerg_num, sizeof(emerg_num));
        nvs_state_get_server_url(server_url, sizeof(server_url));
        int pend_vc = 0;
        bool has_pending = nvs_state_get_pending(&pend_vc, pend_fwid, sizeof(pend_fwid));
        uint32_t last_fail = _get_u32_or_default(K_LAST_FAIL, 0);
        uint8_t fail_count = _get_u8_or_default(K_FAIL_COUNT, 0);

        printf("  companyId:        %s\n", company);
        printf("  productName:      %s\n", product);
        printf("  taxiNumber:       %s\n", taxi_num);
        printf("  smsNumber:        %s\n", sms_num);
        printf("  emergencyNumber:  %s\n", emerg_num);
        printf("  serverUrl:        %s\n", server_url[0] ? server_url : "(unset — using compiled default)");
        if (has_pending) {
            printf("  pending update:   versionCode=%d firmwareId=%s\n", pend_vc, pend_fwid);
        } else {
            printf("  pending update:   (none)\n");
        }
        printf("  lastFailedVersion: %u (streak=%u)\n", (unsigned)last_fail, (unsigned)fail_count);
        return true;
    }

    if (strncmp(p, "company ", 8) == 0) {
        const char *val = p + 8;
        while (*val == ' ') val++;
        if (*val) {
            nvs_state_set_company(val);
            printf("  companyId set to '%s'\n", val);
        } else {
            printf("  usage: nvs company <id>\n");
        }
        return true;
    }

    if (strncmp(p, "product ", 8) == 0) {
        const char *val = p + 8;
        while (*val == ' ') val++;
        if (*val) {
            nvs_state_set_product(val);
            printf("  productName set to '%s'\n", val);
        } else {
            printf("  usage: nvs product <name>\n");
        }
        return true;
    }

    // Added 2026-07-13 (doc 60). ota_client.c no longer writes
    // lastFailedVersionCode/failureCount at all (RAM-only retries now,
    // config.h's OTA_DOWNLOAD_RETRY_COUNT) — this now mainly clears (a)
    // any STALE failure streak a device picked up before that change
    // (from earlier testing, still sitting in NVS from before this
    // firmware was flashed) and (b) a stale pending-update record.
    // companyId/productName provisioning is untouched either way.
    if (strncmp(p, "clear", 5) == 0) {
        nvs_state_clear_failure();
        nvs_state_clear_pending();
        printf("  Cleared any stale failure streak + pending update state (companyId/productName untouched)\n");
        return true;
    }

    printf("  nvs status              Show stored OTA/identity state\n");
    printf("  nvs company <id>        Provision companyId\n");
    printf("  nvs product <name>      Provision productName\n");
    printf("  nvs clear                Clear failure streak + pending state (retry a versionCode that already failed)\n");
    return true;
}
