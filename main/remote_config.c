#include "config.h"

#if ENABLE_REMOTE_CONFIG

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_efuse.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "remote_config.h"
#include "nvs_state.h"

// ================================================================
// remote_config.c — GET REMOTE_CONFIG_PATH, parse 4 string fields, write
// any that changed to NVS. Deliberately mirrors ota_client.c's structure
// (periodic task shape, event-handler-captures-body pattern, serial
// command dispatch) rather than inventing a new one — same
// esp_http_client_perform() primitive already proven for both the small
// OTA JSON calls and (after doc 61) the binary download itself.
// ================================================================

static const char *TAG = "remote_cfg";

static char s_last_check_result[32] = "(none yet)";

// Same cert-bundle logic as ota_client.c's OTA_CRT_BUNDLE_ATTACH — reused
// here rather than duplicated as a separate macro, since it depends only
// on OTA_INSECURE_SKIP_CERT_VERIFY (config.h), not on anything OTA-specific.
#if OTA_INSECURE_SKIP_CERT_VERIFY
  #define REMOTE_CONFIG_CRT_BUNDLE_ATTACH   NULL
#else
  #define REMOTE_CONFIG_CRT_BUNDLE_ATTACH   esp_crt_bundle_attach
#endif

static void _device_id(char *out, size_t out_len) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ─── Response body capture — same principle as ota_client.c's s_resp:
// esp_http_client_perform() drains the response internally unless an
// event handler captures it DURING the call. ──
static char s_resp[512];
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

// ─── Writes a fetched field to NVS ONLY if it actually changed — avoids
// pointless flash writes (NVS wear) on every check when nothing was
// updated by an admin since the last one. ──
static void _apply_if_changed(const char *field_name,
                               void (*get_fn)(char *, size_t), void (*set_fn)(const char *),
                               const char *new_value) {
    char current[128];
    get_fn(current, sizeof(current));
    if (strcmp(current, new_value) != 0) {
        set_fn(new_value);
        ESP_LOGI(TAG, "  %s changed: '%s' -> '%s'", field_name, current, new_value);
    }
}

void remote_config_check_now(void) {
    char device_id[24];
    _device_id(device_id, sizeof(device_id));
    char company_id[48];
    nvs_state_get_company(company_id, sizeof(company_id));

    char path[160];
    snprintf(path, sizeof(path), "%s?macAddress=%s&companyId=%s", REMOTE_CONFIG_PATH, device_id, company_id);

    // Resolved via nvs_state_resolve_server() (doc 72) — same base-URL
    // resolution ota_client.c uses, so remote config bootstraps itself the
    // same way (compiled default unless ENABLE_SERVER_URL_OVERRIDE is on
    // AND something's actually been set).
    char host[96];
    int port;
    bool use_https;
    nvs_state_resolve_server(host, sizeof(host), &port, &use_https);
    char url[300];
    snprintf(url, sizeof(url), "%s://%s:%d%s", use_https ? "https" : "http", host, port, path);

    ESP_LOGI(TAG, "REMOTE CONFIG CHECK -> %s", url);

    esp_err_t err = ESP_FAIL;
    int status = 0;
    for (int attempt = 1; attempt <= REMOTE_CONFIG_RETRY_COUNT; attempt++) {
        s_resp_len = 0;
        memset(s_resp, 0, sizeof(s_resp));
        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = REMOTE_CONFIG_HTTP_TIMEOUT_MS,
            .event_handler = _http_event_handler,
            .crt_bundle_attach = REMOTE_CONFIG_CRT_BUNDLE_ATTACH,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) { err = ESP_FAIL; break; }
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK && status == 200) break;
        bool will_retry = attempt < REMOTE_CONFIG_RETRY_COUNT;
        ESP_LOGW(TAG, "  Check attempt %d/%d failed: %s (status=%d)%s",
                 attempt, REMOTE_CONFIG_RETRY_COUNT, esp_err_to_name(err), status,
                 will_retry ? " — retrying" : " — giving up for this cycle");
        if (will_retry) vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_DELAY_MS));
    }

    if (err != ESP_OK || status != 200) {
        snprintf(s_last_check_result, sizeof(s_last_check_result), "FAILED (status=%d)", status);
        return;
    }

    cJSON *json = cJSON_Parse(s_resp);
    if (!json) {
        ESP_LOGE(TAG, "Response was not valid JSON");
        strlcpy(s_last_check_result, "FAILED (bad JSON)", sizeof(s_last_check_result));
        return;
    }

    cJSON *j_taxi  = cJSON_GetObjectItem(json, "taxiNumber");
    cJSON *j_sms   = cJSON_GetObjectItem(json, "smsNumber");
    cJSON *j_emerg = cJSON_GetObjectItem(json, "emergencyContactNumber");
    cJSON *j_url   = cJSON_GetObjectItem(json, "serverUrl");

    if (cJSON_IsString(j_taxi))  _apply_if_changed("taxiNumber", nvs_state_get_taxi_number, nvs_state_set_taxi_number, j_taxi->valuestring);
    if (cJSON_IsString(j_sms))   _apply_if_changed("smsNumber", nvs_state_get_sms_number, nvs_state_set_sms_number, j_sms->valuestring);
    if (cJSON_IsString(j_emerg)) _apply_if_changed("emergencyContactNumber", nvs_state_get_emergency_number, nvs_state_set_emergency_number, j_emerg->valuestring);
    if (cJSON_IsString(j_url))   _apply_if_changed("serverUrl", nvs_state_get_server_url, nvs_state_set_server_url, j_url->valuestring);

    cJSON_Delete(json);
    strlcpy(s_last_check_result, "OK", sizeof(s_last_check_result));
    ESP_LOGI(TAG, "  Remote config check OK");
}

static void _remote_config_periodic_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(10000)); // stagger past the OTA task's own initial 5s delay
    remote_config_check_now();

    while (1) {
        int jitter_range = (REMOTE_CONFIG_CHECK_INTERVAL_S * OTA_CHECK_JITTER_PERCENT) / 100;
        int jitter = jitter_range > 0
            ? (int)(esp_random() % (uint32_t)(2 * jitter_range + 1)) - jitter_range
            : 0;
        int delay_s = REMOTE_CONFIG_CHECK_INTERVAL_S + jitter;
        if (delay_s < 1) delay_s = 1;

        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
        remote_config_check_now();
    }
}

void remote_config_init(void) {
    xTaskCreate(_remote_config_periodic_task, "remote_cfg", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "Remote config client READY — check every %ds (+/-%d%% jitter)",
             REMOTE_CONFIG_CHECK_INTERVAL_S, OTA_CHECK_JITTER_PERCENT);
}

bool remote_config_process_command(const char *line) {
    while (*line == ' ') line++;
    if (strncmp(line, "config", 6) != 0) return false;

    const char *p = line + 6;
    while (*p == ' ') p++;

    if (strncmp(p, "check", 5) == 0) {
        ESP_LOGI(TAG, "Manual remote-config check requested");
        remote_config_check_now();
        return true;
    }
    if (strncmp(p, "status", 6) == 0) {
        char taxi[32], sms[32], emerg[32], url[128];
        nvs_state_get_taxi_number(taxi, sizeof(taxi));
        nvs_state_get_sms_number(sms, sizeof(sms));
        nvs_state_get_emergency_number(emerg, sizeof(emerg));
        nvs_state_get_server_url(url, sizeof(url));
        printf("  taxiNumber:       %s\n", taxi);
        printf("  smsNumber:        %s\n", sms);
        printf("  emergencyNumber:  %s\n", emerg);
        printf("  serverUrl:        %s\n", url[0] ? url : "(unset)");
        printf("  serverUrlOverride: %s\n", ENABLE_SERVER_URL_OVERRIDE ? "ENABLED" : "disabled (config.h)");
        printf("  Last check result: %s\n", s_last_check_result);
        return true;
    }

    printf("  config check    Check the server right now\n");
    printf("  config status    Show current remote-config values + last check result\n");
    return true;
}

#else // !ENABLE_REMOTE_CONFIG

#include "esp_log.h"
#include "remote_config.h"

void remote_config_init(void) { ESP_LOGI("remote_cfg", "DISABLED (ENABLE_REMOTE_CONFIG=0 in config.h)"); }
void remote_config_check_now(void) {}
bool remote_config_process_command(const char *line) { (void)line; return false; }

#endif // ENABLE_REMOTE_CONFIG
