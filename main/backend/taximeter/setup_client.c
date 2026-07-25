/**
 * setup_client.c — Network passcode + vehicle lookup
 *
 * See setup_client.h for the endpoint shapes and why Vehicle is parsed
 * differently from Network (one is Outcome<T>-wrapped, the other isn't
 * — a real asymmetry in the reference API, not a bug here).
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "config.h"
#include "api_client.h"
#include "session_store.h"
#include "setup_client.h"
#include "bg_worker.h"

static const char *TAG = "setup";

esp_err_t setup_client_resolve_network(const char *passcode) {
    if (!passcode || passcode[0] == '\0') {
        ESP_LOGE(TAG, "resolve_network: passcode cannot be empty");
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s?passcode=%s", EP_NETWORK, passcode);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "NETWORK LOOKUP — passcode=%s", passcode);

    // Heap-allocated, not static — a static buffer here permanently
    // reserves DRAM for the program's whole lifetime.
    char *resp = malloc(API_SMALL_BUFFER_SIZE);
    if (!resp) {
        ESP_LOGE(TAG, "NETWORK LOOKUP: malloc(%d) failed — out of heap right now (see 'mem')", API_SMALL_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = api_client_request(API_METHOD_GET, path, NULL, false, resp, API_SMALL_BUFFER_SIZE, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NETWORK LOOKUP failed — no response (see error above)");
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return err;
    }

    cJSON *json = cJSON_Parse(resp);
    cJSON *success = json ? cJSON_GetObjectItemCaseSensitive(json, "success") : NULL;
    cJSON *data    = json ? cJSON_GetObjectItemCaseSensitive(json, "data") : NULL;

    if (!json || !cJSON_IsTrue(success) || !cJSON_IsObject(data)) {
        cJSON *message = json ? cJSON_GetObjectItemCaseSensitive(json, "message") : NULL;
        ESP_LOGE(TAG, "NETWORK LOOKUP FAILED (HTTP %d): %s", status,
                 cJSON_IsString(message) ? message->valuestring : "invalid/unexpected response");
        if (json) cJSON_Delete(json);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    cJSON *network_id    = cJSON_GetObjectItemCaseSensitive(data, "networkId");
    cJSON *company_name  = cJSON_GetObjectItemCaseSensitive(data, "companyName");
    cJSON *active        = cJSON_GetObjectItemCaseSensitive(data, "active");

    if (!cJSON_IsNumber(network_id)) {
        ESP_LOGE(TAG, "NETWORK LOOKUP: response missing 'networkId' — raw: %.200s", resp);
        cJSON_Delete(json);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    session_store_set_network((int64_t)network_id->valuedouble,
                               cJSON_IsString(company_name) ? company_name->valuestring : NULL);

    ESP_LOGI(TAG, "NETWORK OK \xE2\x9C\x93 — network_id=%lld company=\"%s\" active=%s",
             (long long)session_store_get_network_id(),
             cJSON_IsString(company_name) ? company_name->valuestring : "?",
             cJSON_IsTrue(active) ? "yes" : "no");
    cJSON_Delete(json);
    free(resp);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ESP_OK;
}

esp_err_t setup_client_resolve_vehicle(const char *vehicle_no) {
    if (!vehicle_no || vehicle_no[0] == '\0') {
        ESP_LOGE(TAG, "resolve_vehicle: vehicle number cannot be empty");
        return ESP_ERR_INVALID_ARG;
    }
    int64_t network_id = session_store_get_network_id();
    if (network_id <= 0) {
        ESP_LOGE(TAG, "resolve_vehicle: no network resolved yet — run 'setup network' first");
        return ESP_ERR_INVALID_STATE;
    }

    char path[160];
    snprintf(path, sizeof(path), "%s?networkId=%lld&vehicleNo=%s", EP_VEHICLE, (long long)network_id, vehicle_no);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "VEHICLE LOOKUP — networkId=%lld vehicleNo=%s", (long long)network_id, vehicle_no);

    char *resp = malloc(API_SMALL_BUFFER_SIZE);
    if (!resp) {
        ESP_LOGE(TAG, "VEHICLE LOOKUP: malloc(%d) failed — out of heap right now (see 'mem')", API_SMALL_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = api_client_request(API_METHOD_GET, path, NULL, false, resp, API_SMALL_BUFFER_SIZE, &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "VEHICLE LOOKUP failed — no response (see error above)");
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return err;
    }

    // NOT Outcome<T>-wrapped — the server returns the VehicleDto object
    // directly. An empty/"null" body or non-200 means "not found".
    if (status != 200 || resp[0] == '\0' || strcmp(resp, "null") == 0) {
        ESP_LOGE(TAG, "VEHICLE LOOKUP: not found (HTTP %d) — check vehicleNo/networkId", status);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(resp);
    if (!json) {
        ESP_LOGE(TAG, "VEHICLE LOOKUP: response is not valid JSON — raw: %.200s", resp);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    cJSON *id             = cJSON_GetObjectItemCaseSensitive(json, "id");
    cJSON *veh_no         = cJSON_GetObjectItemCaseSensitive(json, "vehicleNo");
    cJSON *vehicle_type_id = cJSON_GetObjectItemCaseSensitive(json, "vehicleTypeId");
    cJSON *active         = cJSON_GetObjectItemCaseSensitive(json, "active");

    if (!cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "VEHICLE LOOKUP: response missing 'id' — raw: %.200s", resp);
        cJSON_Delete(json);
        free(resp);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return ESP_FAIL;
    }

    session_store_set_vehicle((int64_t)id->valuedouble,
                               cJSON_IsNumber(vehicle_type_id) ? (int64_t)vehicle_type_id->valuedouble : 0,
                               cJSON_IsString(veh_no) ? veh_no->valuestring : vehicle_no);

    ESP_LOGI(TAG, "VEHICLE OK \xE2\x9C\x93 — vehicle_id=%lld vehicle_type_id=%lld active=%s",
             (long long)session_store_get_vehicle_id(), (long long)session_store_get_vehicle_type_id(),
             cJSON_IsTrue(active) ? "yes" : "no");
    cJSON_Delete(json);
    free(resp);
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ESP_OK;
}

void setup_client_run_if_needed(void) {
    if (session_store_get_network_id() <= 0) {
        setup_client_resolve_network(SETUP_NETWORK_PASSCODE);
    }
    if (session_store_get_vehicle_id() <= 0 && session_store_get_network_id() > 0) {
        char vehicle_no[32];
        session_store_get_vehicle_no(vehicle_no, sizeof(vehicle_no));
        setup_client_resolve_vehicle(vehicle_no[0] ? vehicle_no : SETUP_VEHICLE_NO);
    }
}

// bg_worker job wrappers for the "setup network"/"setup vehicle" serial
// commands — see auth_client.c's matching comment for why: the "serial_cmd"
// task's stack isn't deep enough for an mbedTLS handshake, so the actual
// HTTPS call runs on bg_worker's 8KB-stack task instead.
// setup_client_run_if_needed() (called at boot, on the main task) keeps
// calling setup_client_resolve_network()/_vehicle() directly — no
// routing needed there, only from this file's own serial commands.
typedef struct {
    char value[32];
} _setup_job_arg_t;

static bool _network_job(void *arg) {
    _setup_job_arg_t *a = (_setup_job_arg_t *)arg;
    esp_err_t err = setup_client_resolve_network(a->value);
    free(a);
    return err == ESP_OK;
}

static bool _vehicle_job(void *arg) {
    _setup_job_arg_t *a = (_setup_job_arg_t *)arg;
    esp_err_t err = setup_client_resolve_vehicle(a->value);
    free(a);
    return err == ESP_OK;
}

static void _show_help(void) {
    printf("\n  setup network [passcode]   Resolve network (default: config.h SETUP_NETWORK_PASSCODE)\n");
    printf("  setup vehicle [vehicleNo]  Resolve vehicle (default: config.h SETUP_VEHICLE_NO)\n");
    printf("  setup info                 Show what's currently resolved\n");
    printf("  setup help                 Show this help\n\n");
}

bool setup_client_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "setup", 5) != 0) return false;

    const char *p = line + 5;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) {
        _show_help();
    } else if (strncmp(p, "network", 7) == 0) {
        _setup_job_arg_t *a = malloc(sizeof(*a));
        if (!a) {
            ESP_LOGE(TAG, "setup network: malloc failed — out of heap right now (see 'mem')");
        } else {
            strlcpy(a->value, SETUP_NETWORK_PASSCODE, sizeof(a->value));
            sscanf(p + 7, "%31s", a->value);
            if (!bg_worker_submit_fn(_network_job, a, NULL, NULL)) {
                ESP_LOGW(TAG, "setup network: background worker busy — try again shortly");
                free(a);
            } else {
                ESP_LOGI(TAG, "setup network: queued on background worker");
            }
        }
    } else if (strncmp(p, "vehicle", 7) == 0) {
        _setup_job_arg_t *a = malloc(sizeof(*a));
        if (!a) {
            ESP_LOGE(TAG, "setup vehicle: malloc failed — out of heap right now (see 'mem')");
        } else {
            strlcpy(a->value, SETUP_VEHICLE_NO, sizeof(a->value));
            sscanf(p + 7, "%31s", a->value);
            if (!bg_worker_submit_fn(_vehicle_job, a, NULL, NULL)) {
                ESP_LOGW(TAG, "setup vehicle: background worker busy — try again shortly");
                free(a);
            } else {
                ESP_LOGI(TAG, "setup vehicle: queued on background worker");
            }
        }
    } else if (strcmp(p, "info") == 0) {
        char vehicle_no[32] = {0};
        session_store_get_vehicle_no(vehicle_no, sizeof(vehicle_no));
        ESP_LOGI(TAG, "network_id=%lld vehicle_id=%lld vehicle_type_id=%lld vehicle_no=%s",
                 (long long)session_store_get_network_id(), (long long)session_store_get_vehicle_id(),
                 (long long)session_store_get_vehicle_type_id(), vehicle_no);
    } else {
        printf("Unknown 'setup' subcommand. Type 'setup help'.\n");
    }
    return true;
}
