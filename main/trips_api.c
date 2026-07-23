#include "config.h"

#if ENABLE_TRIPS_API

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_spiffs.h"
#include "trips_api.h"

static const char *TAG = "trips_api";

static bool s_mounted = false;
static char s_file_path[256];
static FILE *s_file_handle = NULL;
static int  s_bytes_written = 0;
static int  s_total_fetches = 0, s_total_ok = 0, s_total_fails = 0;

// ── Heap logging around the HTTPS call — same diagnostic pattern used
// all through esp32_display_taxi's rest_api_storage.c this session, so
// any TLS/memory issue here is just as easy to diagnose from the log. ──
static void _log_heap(const char *when) {
    ESP_LOGI(TAG, "  Heap %-18s free=%u KB | largest free block=%u KB", when,
             (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
}

esp_err_t trips_api_init(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount FAILED (%s) — trips API storage unavailable", esp_err_to_name(ret));
        return ret;
    }
    s_mounted = true;

    struct stat st;
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "/spiffs%s", STORAGE_DIR);
    if (stat(dir_path, &st) != 0) mkdir(dir_path, 0755);

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "Trips API READY — SPIFFS: %u KB total | %u KB used | %u KB free",
             (unsigned)(total / 1024), (unsigned)(used / 1024), (unsigned)((total - used) / 1024));
    return ESP_OK;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP error event");
            break;
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0 && s_file_handle) {
                size_t w = fwrite(evt->data, 1, evt->data_len, s_file_handle);
                if ((int)w == evt->data_len) {
                    s_bytes_written += evt->data_len;
                } else {
                    ESP_LOGE(TAG, "SPIFFS write short: wanted %d, wrote %d", evt->data_len, (int)w);
                    return ESP_FAIL;
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t trips_api_fetch(int trip_id) {
    if (!s_mounted) {
        ESP_LOGE(TAG, "Storage not mounted — call trips_api_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://%s%s/%d", TRIPS_API_HOST, TRIPS_API_PATH, trip_id);

    s_total_fetches++;
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "FETCH #%d -> Trip ID: %d", s_total_fetches, trip_id);
    ESP_LOGI(TAG, "  URL: %s", url);
    _log_heap("before HTTP init");

    snprintf(s_file_path, sizeof(s_file_path), "/spiffs%s/trips_%d.json", STORAGE_DIR, trip_id);
    s_bytes_written = 0;
    s_file_handle = fopen(s_file_path, "w");
    if (!s_file_handle) {
        ESP_LOGE(TAG, "Failed to open %s for writing", s_file_path);
        s_total_fails++;
        return ESP_FAIL;
    }

    esp_http_client_config_t http_cfg = {
        .url                = url,
        .method             = HTTP_METHOD_GET,
        .timeout_ms         = TRIPS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .event_handler      = _http_event_handler,
        .buffer_size        = TRIPS_BUFFER_SIZE,
        .keep_alive_enable  = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        fclose(s_file_handle); s_file_handle = NULL;
        ESP_LOGE(TAG, "HTTP client init FAILED");
        s_total_fails++;
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ESP32-TripsAPI-Client/1.0");

    // ── Trips-API-specific auth (added for esp32s3_board) ───────────
    // This Authorization header is scoped ONLY to this one GET endpoint
    // (TRIPS_API_HOST+TRIPS_API_PATH) — it is NOT a general/shared token
    // used by any other API call in this codebase (OTA's Manifest/Report
    // and remote-config's own endpoint send no auth at all). If a future
    // module needs its own token, it should define its own
    // <MODULE>_AUTH_ENABLED / <MODULE>_AUTH_TOKEN pair in config.h rather
    // than reusing this one. See config.h's TRIPS_API_AUTH_ENABLED /
    // TRIPS_API_AUTH_TOKEN for the value and the "already expired,
    // replace before enabling" note.
#if TRIPS_API_AUTH_ENABLED
    char auth_header[600];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", TRIPS_API_AUTH_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth_header);
#endif

    _log_heap("right before perform()");
    esp_err_t err = esp_http_client_perform(client);
    _log_heap("right after perform()");
    int status = esp_http_client_get_status_code(client);

    fclose(s_file_handle);
    s_file_handle = NULL;

    if (err == ESP_OK && status == 200) {
        s_total_ok++;
        ESP_LOGI(TAG, "OK HTTP %d | %d bytes -> %s", status, s_bytes_written, s_file_path);
    } else {
        s_total_fails++;
        ESP_LOGE(TAG, "FAILED: %s (status=%d) — removing incomplete file",
                 esp_err_to_name(err), status);
        remove(s_file_path);
    }
    ESP_LOGI(TAG, "Stats: %d fetches | %d OK | %d failed", s_total_fetches, s_total_ok, s_total_fails);
    ESP_LOGI(TAG, "══════════════════════════════════════");

    esp_http_client_cleanup(client);
    return (err == ESP_OK && status == 200) ? ESP_OK : ESP_FAIL;
}

static void _read_and_print(int trip_id) {
    char path[256];
    snprintf(path, sizeof(path), "/spiffs%s/trips_%d.json", STORAGE_DIR, trip_id);

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "Not found: %s (use 'api get %d' first)", path, trip_id);
        return;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) { ESP_LOGE(TAG, "Failed to open %s", path); return; }

    ESP_LOGI(TAG, "%s (%ld bytes):", path, (long)st.st_size);
    char buf[512];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf) - 1, fp)) > 0) {
        buf[r] = '\0';
        printf("%s", buf);
    }
    printf("\n");
    fclose(fp);
}

static void _list_files(void) {
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "/spiffs%s", STORAGE_DIR);

    DIR *dir = opendir(dir_path);
    if (!dir) { ESP_LOGW(TAG, "No files stored yet ('api get <id>' first)"); return; }

    int count = 0;
    long total_bytes = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < STORAGE_MAX_FILES) {
        if (entry->d_type != DT_REG) continue;
        if (strncmp(entry->d_name, "trips_", 6) != 0) continue;

        char full_path[400];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        count++;
        total_bytes += st.st_size;
        ESP_LOGI(TAG, "  [%2d] %-24s %7ld bytes", count, entry->d_name, (long)st.st_size);
    }
    closedir(dir);

    if (count == 0) ESP_LOGI(TAG, "  No trip files stored");
    else ESP_LOGI(TAG, "  Total: %d file(s) | %ld bytes", count, total_bytes);
}

static void _delete(int trip_id, bool all) {
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "/spiffs%s", STORAGE_DIR);

    if (all) {
        DIR *dir = opendir(dir_path);
        if (!dir) { ESP_LOGW(TAG, "Cannot open %s", dir_path); return; }
        int deleted = 0;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type != DT_REG) continue;
            if (strncmp(entry->d_name, "trips_", 6) != 0) continue;
            char full_path[400];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
            if (remove(full_path) == 0) deleted++;
        }
        closedir(dir);
        ESP_LOGI(TAG, "Deleted %d file(s)", deleted);
    } else {
        char path[256];
        snprintf(path, sizeof(path), "%s/trips_%d.json", dir_path, trip_id);
        if (remove(path) == 0) ESP_LOGI(TAG, "Deleted %s", path);
        else ESP_LOGW(TAG, "Not found: %s", path);
    }
}

static void _show_info(void) {
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "Backend: SPIFFS | %u/%u KB used | Fetch stats: %d total, %d OK, %d failed | Auth: %s",
             (unsigned)(used / 1024), (unsigned)(total / 1024),
             s_total_fetches, s_total_ok, s_total_fails,
             TRIPS_API_AUTH_ENABLED ? "ON (trip-scoped token)" : "OFF");
}

static void _show_help(void) {
    printf("\n  api get [id]      Fetch trip JSON, store to SPIFFS (default id: %d)\n", TRIPS_DEFAULT_ID);
    printf("  api read [id]     Print a stored trip's JSON\n");
    printf("  api list          List all stored trip files\n");
    printf("  api delete <id|all>  Delete one file, or all of them\n");
    printf("  api info          Storage backend + free space + stats\n");
    printf("  api help          This list\n\n");
}

bool trips_api_process_command(const char *line) {
    while (*line == ' ') line++;
    if (strncmp(line, "api", 3) != 0) return false;

    const char *p = line + 3;
    while (*p == ' ') p++;
    if (*p == '\0') { _show_help(); return true; }

    char cmd[16] = {0};
    int ci = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && ci < 15) cmd[ci++] = *p++;
    while (*p == ' ') p++;

    char arg[32] = {0};
    int ai = 0;
    while (*p && *p != '\r' && *p != '\n' && ai < 31) arg[ai++] = *p++;

    int trip_id = TRIPS_DEFAULT_ID;
    bool want_all = (strcmp(arg, "all") == 0);
    if (arg[0] != '\0' && !want_all) {
        int v = atoi(arg);
        if (v > 0) trip_id = v;
    }

    if (strcmp(cmd, "get") == 0)    { trips_api_fetch(trip_id); return true; }
    if (strcmp(cmd, "read") == 0)   { _read_and_print(trip_id); return true; }
    if (strcmp(cmd, "list") == 0)   { _list_files(); return true; }
    if (strcmp(cmd, "delete") == 0) { _delete(trip_id, want_all); return true; }
    if (strcmp(cmd, "info") == 0)   { _show_info(); return true; }
    if (strcmp(cmd, "help") == 0)   { _show_help(); return true; }

    ESP_LOGW(TAG, "Unknown api command '%s' — try 'api help'", cmd);
    return true;
}

#else // !ENABLE_TRIPS_API

#include "esp_log.h"
#include "trips_api.h"

esp_err_t trips_api_init(void) {
    ESP_LOGI("trips_api", "DISABLED (ENABLE_TRIPS_API=0 in config.h)");
    return ESP_OK;
}
esp_err_t trips_api_fetch(int trip_id) { (void)trip_id; return ESP_ERR_NOT_SUPPORTED; }
bool trips_api_process_command(const char *line) { (void)line; return false; }

#endif // ENABLE_TRIPS_API
