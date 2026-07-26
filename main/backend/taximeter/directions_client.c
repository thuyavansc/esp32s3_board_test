/**
 * directions_client.c — Road-snapped distance (GraphHopper)
 *
 * See directions_client.h for the full design. This is a standalone
 * HTTPS client (like api_client.c's _perform(), same TLS/buffer/heap-
 * logging idiom) rather than reusing api_client_request() directly —
 * that helper hardcodes TAXIMETER_API_HOST, but GraphHopper is a
 * different host with no Bearer auth (a query-string API key instead),
 * so it needs its own small _perform()-equivalent.
 *
 * Requests points_encoded=false (a plain GeoJSON coordinate array)
 * instead of GraphHopper's default Google-polyline-encoded string —
 * deliberately, not an oversight: decoding Google's polyline format
 * needs its own bit-packing decoder, while a GeoJSON coordinate array
 * is a plain JSON array of [lon,lat] pairs cJSON already parses for
 * free. Response bodies are a few hundred points at most for a normal
 * gap, well within DIRECTIONS_BUFFER_SIZE.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "cJSON.h"
#include "config.h"
#include "directions_client.h"

static const char *TAG = "directions";

// ── Response buffer sink (bounded, no heap streaming) — same shape as
//    api_client.c's response_sink_t, kept separate since this module
//    talks to a different host with different auth ──
typedef struct {
    char  *buf;
    size_t buf_size;
    size_t buf_used;
    bool   truncated;
} _sink_t;

static esp_err_t _event_handler(esp_http_client_event_t *evt) {
    _sink_t *sink = (_sink_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0 || !sink) return ESP_OK;

    size_t space = (sink->buf_size > sink->buf_used + 1) ? (sink->buf_size - sink->buf_used - 1) : 0;
    size_t to_copy = ((size_t)evt->data_len < space) ? (size_t)evt->data_len : space;
    if (to_copy > 0) {
        memcpy(sink->buf + sink->buf_used, evt->data, to_copy);
        sink->buf_used += to_copy;
    }
    if (to_copy < (size_t)evt->data_len) sink->truncated = true;
    return ESP_OK;
}

bool directions_client_get_route(double from_lat, double from_lon, double to_lat, double to_lon,
                                  double *out_distance_m,
                                  directions_point_t *out_points, int out_points_max, int *out_point_count) {
    if (out_point_count) *out_point_count = 0;

    if (!DIRECTIONS_ENABLED) {
        ESP_LOGI(TAG, "get_route: DIRECTIONS_ENABLED=0 — caller should fall back to straight-line");
        return false;
    }
    if (GRAPHHOPPER_API_KEY[0] == '\0') {
        ESP_LOGW(TAG, "get_route: no GRAPHHOPPER_API_KEY set (config.h) — falling back to straight-line. "
                 "Get a free-tier key at graphhopper.com if road-snapped distance is needed.");
        return false;
    }
    if (!out_distance_m || !out_points || out_points_max <= 0) return false;

    char url[400];
    snprintf(url, sizeof(url),
             "https://%s%s?point=%.6f,%.6f&point=%.6f,%.6f&vehicle=car&points_encoded=false&key=%s",
             DIRECTIONS_API_HOST, DIRECTIONS_API_PATH, from_lat, from_lon, to_lat, to_lon, GRAPHHOPPER_API_KEY);

    char *resp = malloc(DIRECTIONS_BUFFER_SIZE);
    if (!resp) {
        ESP_LOGE(TAG, "get_route: malloc(%d) for response buffer FAILED — out of heap right now (see 'mem')",
                 DIRECTIONS_BUFFER_SIZE);
        return false;
    }
    _sink_t sink = { .buf = resp, .buf_size = DIRECTIONS_BUFFER_SIZE };

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = DIRECTIONS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = _event_handler,
        .user_data         = &sink,
        .buffer_size       = DIRECTIONS_BUFFER_SIZE,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "get_route: esp_http_client_init() FAILED (out of memory?)");
        free(resp);
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    ESP_LOGI(TAG, "-> GET %s/route (from=%.6f,%.6f to=%.6f,%.6f)", DIRECTIONS_API_HOST, from_lat, from_lon, to_lat, to_lon);
    size_t free_before = esp_get_free_heap_size();
    esp_err_t err = esp_http_client_perform(client);
    ESP_LOGI(TAG, "   heap before=%uKB after=%uKB", (unsigned)(free_before / 1024),
             (unsigned)(esp_get_free_heap_size() / 1024));

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_route: HTTP transport failed (%s) — falling back to straight-line", esp_err_to_name(err));
        free(resp);
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "get_route: HTTP %d (not 200) — falling back to straight-line. Body (first 200): %.200s",
                 status, resp);
        free(resp);
        return false;
    }
    resp[sink.buf_used] = '\0';
    if (sink.truncated) {
        ESP_LOGW(TAG, "get_route: response TRUNCATED at %u bytes — route may be too long for DIRECTIONS_BUFFER_SIZE; "
                 "distance/points below may be incomplete", (unsigned)sink.buf_used);
    }

    cJSON *json = cJSON_Parse(resp);
    free(resp);
    if (!json) {
        ESP_LOGW(TAG, "get_route: response is not valid JSON — falling back to straight-line");
        return false;
    }

    cJSON *paths = cJSON_GetObjectItemCaseSensitive(json, "paths");
    cJSON *path0 = cJSON_IsArray(paths) ? cJSON_GetArrayItem(paths, 0) : NULL;
    if (!cJSON_IsObject(path0)) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(json, "message");
        ESP_LOGW(TAG, "get_route: no route found — %s — falling back to straight-line",
                 cJSON_IsString(message) ? message->valuestring : "(no 'paths' in response)");
        cJSON_Delete(json);
        return false;
    }

    cJSON *distance = cJSON_GetObjectItemCaseSensitive(path0, "distance");
    if (!cJSON_IsNumber(distance) || distance->valuedouble < 0.0) {
        ESP_LOGW(TAG, "get_route: response missing a valid 'distance' field — falling back to straight-line");
        cJSON_Delete(json);
        return false;
    }
    *out_distance_m = distance->valuedouble;

    // points_encoded=false -> points.coordinates is [[lon,lat], [lon,lat], ...]
    // (GeoJSON order is lon,lat — NOT lat,lon; easy to get backwards).
    int count = 0;
    cJSON *points = cJSON_GetObjectItemCaseSensitive(path0, "points");
    cJSON *coords = cJSON_IsObject(points) ? cJSON_GetObjectItemCaseSensitive(points, "coordinates") : NULL;
    if (cJSON_IsArray(coords)) {
        cJSON *pair;
        cJSON_ArrayForEach(pair, coords) {
            if (count >= out_points_max) {
                ESP_LOGW(TAG, "get_route: route has more points than out_points_max (%d) — extra points dropped",
                         out_points_max);
                break;
            }
            if (!cJSON_IsArray(pair)) continue;
            cJSON *lon_j = cJSON_GetArrayItem(pair, 0);
            cJSON *lat_j = cJSON_GetArrayItem(pair, 1);
            if (!cJSON_IsNumber(lon_j) || !cJSON_IsNumber(lat_j)) continue;
            out_points[count].lat = lat_j->valuedouble;
            out_points[count].lon = lon_j->valuedouble;
            count++;
        }
    }
    if (out_point_count) *out_point_count = count;

    ESP_LOGI(TAG, "get_route: OK — road distance=%.1fm, %d shape point(s)", *out_distance_m, count);
    cJSON_Delete(json);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════
bool directions_client_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "directions", 10) != 0) return false;

    const char *p = line + 10;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) {
        printf("\n  directions test <lat1> <lon1> <lat2> <lon2>   Call GraphHopper directly, print result\n");
        printf("  directions help\n\n");
        return true;
    }
    if (strncmp(p, "test", 4) == 0) {
        double lat1, lon1, lat2, lon2;
        if (sscanf(p + 4, "%lf %lf %lf %lf", &lat1, &lon1, &lat2, &lon2) != 4) {
            printf("Usage: directions test <lat1> <lon1> <lat2> <lon2>\n");
            return true;
        }
        double dist_m = 0.0;
        directions_point_t pts[FARE_CALC_MAX_POINTS_PER_FRAME];
        int count = 0;
        bool ok = directions_client_get_route(lat1, lon1, lat2, lon2, &dist_m, pts, FARE_CALC_MAX_POINTS_PER_FRAME, &count);
        if (ok) {
            ESP_LOGI(TAG, "directions test: OK — %.1fm, %d points (first: %.6f,%.6f)",
                     dist_m, count, count > 0 ? pts[0].lat : 0.0, count > 0 ? pts[0].lon : 0.0);
        } else {
            ESP_LOGW(TAG, "directions test: failed — see log above (fell back to straight-line is the expected caller behavior)");
        }
        return true;
    }

    printf("Unknown 'directions' subcommand. Type 'directions help'.\n");
    return true;
}
