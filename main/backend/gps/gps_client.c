/**
 * gps_client.c — GPS dispatcher (see gps_client.h for the full design).
 *
 * Owns one fix slot per backend (GNSS, NEO-6M, injection) so all
 * enabled backends can run in parallel without overwriting each other;
 * hands whichever slot is the current "active source" to fare_calc.c,
 * with an automatic fallback to an injected fix if the active source
 * has no fix yet. "gps set ..." (injection) is handled directly here,
 * unconditionally, so it always works regardless of which backends are
 * compiled/enabled.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/uart.h"   // UART_NUM_1/UART_NUM_2 (GNSS_UART_NUM/NEO6M_UART_NUM) referenced in the log lines below
#include "esp_log.h"
#include "config.h"
#include "gps_client.h"

#if ENABLE_GPS_GNSS
#include "gps_backend_gnss.h"
#endif
#if ENABLE_GPS_NEO6M
#include "gps_backend_neo6m.h"
#endif

static const char *TAG = "gps";

typedef struct {
    gps_data_t data;
    bool       has_fix;
} _fix_slot_t;

static _fix_slot_t   s_gnss   = {0};
static _fix_slot_t   s_neo6m  = {0};
static _fix_slot_t   s_inject = {0};
static gps_source_t  s_active = GPS_DEFAULT_ACTIVE_SOURCE;
static bool          s_running = false;

void gps_client_publish_gnss_fix(const gps_data_t *fix) {
    if (!fix) return;
    s_gnss.data = *fix;
    s_gnss.has_fix = fix->has_fix;
}

void gps_client_publish_neo6m_fix(const gps_data_t *fix) {
    if (!fix) return;
    s_neo6m.data = *fix;
    s_neo6m.has_fix = fix->has_fix;
}

void gps_client_set_active_source(gps_source_t src) { s_active = src; }
gps_source_t gps_client_get_active_source(void)     { return s_active; }

static const char *_source_name(gps_source_t src) {
    switch (src) {
        case GPS_SRC_GNSS:   return "GNSS";
        case GPS_SRC_NEO6M:  return "NEO-6M";
        case GPS_SRC_INJECT: return "INJECT";
        default:             return "?";
    }
}

const gps_data_t *gps_client_get_latest(void) {
    _fix_slot_t *primary = NULL;
    switch (s_active) {
        case GPS_SRC_GNSS:   primary = &s_gnss;   break;
        case GPS_SRC_NEO6M:  primary = &s_neo6m;  break;
        case GPS_SRC_INJECT: primary = &s_inject; break;
    }
    if (primary && primary->has_fix) return &primary->data;

    // Active source has no fix yet — fall back to an injected fix if one
    // exists (lets the PC GUI drive a trip even while GNSS is still cold-
    // starting, per doc 139 §10 decision B).
    if (s_active != GPS_SRC_INJECT && s_inject.has_fix) return &s_inject.data;

    return NULL;
}

const char *gps_client_get_status(void) {
    if (!s_running) return "DISABLED";
    return gps_client_get_latest() ? "FIX OK" : "NO FIX";
}

bool gps_client_is_running(void) { return s_running; }

esp_err_t gps_client_init(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "GPS Client Init — default active source: %s", _source_name(s_active));

#if ENABLE_GPS_GNSS
    ESP_LOGI(TAG, "  Starting GNSS backend (A7670E, UART%d)...", GNSS_UART_NUM);
    gps_backend_gnss_init();
#else
    ESP_LOGI(TAG, "  GNSS backend NOT compiled in (ENABLE_GPS_GNSS=0)");
#endif

#if ENABLE_GPS_NEO6M
    ESP_LOGI(TAG, "  Starting NEO-6M backend (UART%d)...", NEO6M_UART_NUM);
    gps_backend_neo6m_init();
#else
    ESP_LOGI(TAG, "  NEO-6M backend NOT compiled in (ENABLE_GPS_NEO6M=0)");
#endif

    ESP_LOGI(TAG, "  Injection ('gps set ...') always available regardless of the above.");
    s_running = true;
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  "gps set <lat> <lon> <speed_kmh> [hdop] [sats]" — always available,
//  regardless of which backends are compiled/enabled. Publishes a fix
//  exactly as if a real receiver had produced it — fare_calc.c can't
//  tell the difference.
// ═══════════════════════════════════════════════════════════════
static bool _cmd_set(const char *args) {
    double lat = 0, lon = 0, speed = 0, hdop = 1.0;
    int sats = 6;
    int n = sscanf(args, "%lf %lf %lf %lf %d", &lat, &lon, &speed, &hdop, &sats);
    if (n < 3) {
        printf("Usage: gps set <lat> <lon> <speed_kmh> [hdop] [sats]\n");
        return true;
    }

    s_inject.data = (gps_data_t){
        .lat = lat, .lon = lon, .speed = speed, .hdop = hdop,
        .satellites = sats, .fix_quality = 1, .has_fix = true,
        .alt = 0, .course = 0,
    };
    s_inject.has_fix = true;
    ESP_LOGI(TAG, "\xE2\x96\xB6 GPS INJECTED — lat=%.6f lon=%.6f speed=%.1fkm/h hdop=%.1f sats=%d",
             lat, lon, speed, hdop, sats);
    return true;
}

static bool _cmd_source(const char *args) {
    if (strcmp(args, "gnss") == 0)   { gps_client_set_active_source(GPS_SRC_GNSS);   ESP_LOGI(TAG, "Active GPS source -> GNSS");   return true; }
    if (strcmp(args, "neo6m") == 0)  { gps_client_set_active_source(GPS_SRC_NEO6M);  ESP_LOGI(TAG, "Active GPS source -> NEO-6M"); return true; }
    if (strcmp(args, "inject") == 0) { gps_client_set_active_source(GPS_SRC_INJECT); ESP_LOGI(TAG, "Active GPS source -> INJECT"); return true; }
    printf("Usage: gps source gnss|neo6m|inject\n");
    return true;
}

static void _cmd_info(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "GPS DISPATCHER STATUS");
    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  Active source:  %s", _source_name(s_active));
#if ENABLE_GPS_GNSS
    ESP_LOGI(TAG, "  GNSS backend:   compiled in | fix=%s", s_gnss.has_fix ? "YES" : "NO");
#else
    ESP_LOGI(TAG, "  GNSS backend:   not compiled in");
#endif
#if ENABLE_GPS_NEO6M
    ESP_LOGI(TAG, "  NEO-6M backend: compiled in | fix=%s", s_neo6m.has_fix ? "YES" : "NO");
#else
    ESP_LOGI(TAG, "  NEO-6M backend: not compiled in");
#endif
    ESP_LOGI(TAG, "  Injected fix:   %s", s_inject.has_fix ? "YES" : "NO");

    const gps_data_t *cur = gps_client_get_latest();
    if (cur) {
        ESP_LOGI(TAG, "  ──────────────────────────────────");
        ESP_LOGI(TAG, "  Currently feeding fare_calc:");
        ESP_LOGI(TAG, "    lat=%.6f lon=%.6f speed=%.1fkm/h sats=%d hdop=%.1f",
                 cur->lat, cur->lon, cur->speed, cur->satellites, cur->hdop);
    } else {
        ESP_LOGI(TAG, "  Currently feeding fare_calc: NO FIX from any source");
    }
    ESP_LOGI(TAG, "  Try: gps source gnss|neo6m|inject | gps gnss ... | gps neo6m ...");
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

bool gps_client_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "gps ", 4) != 0 && strcmp(line, "gps") != 0) return false;

    const char *p = line + 3;
    while (*p == ' ') p++;

    if (*p == '\0') {
        ESP_LOGI(TAG, "GPS Commands: gps set <lat> <lon> <speed> [hdop] [sats] | gps info"
                       " | gps source gnss|neo6m|inject | gps gnss ... | gps neo6m ...");
        return true;
    }

    char cmd[16] = {0};
    const char *q = p;
    int ci = 0;
    while (*q && *q != ' ' && ci < 15) cmd[ci++] = *q++;
    cmd[ci] = '\0';
    while (*q == ' ') q++;

    if (strcmp(cmd, "set") == 0)    return _cmd_set(q);
    if (strcmp(cmd, "info") == 0)   { _cmd_info(); return true; }
    if (strcmp(cmd, "source") == 0) return _cmd_source(q);

    if (strcmp(cmd, "gnss") == 0) {
#if ENABLE_GPS_GNSS
        return gps_backend_gnss_process_command(q);
#else
        ESP_LOGW(TAG, "GNSS backend not compiled in (ENABLE_GPS_GNSS=0 in config.h)");
        return true;
#endif
    }

    if (strcmp(cmd, "neo6m") == 0) {
#if ENABLE_GPS_NEO6M
        return gps_backend_neo6m_process_command(q);
#else
        ESP_LOGW(TAG, "NEO-6M backend not compiled in (ENABLE_GPS_NEO6M=0 in config.h)");
        return true;
#endif
    }

    ESP_LOGW(TAG, "Unknown 'gps' command: '%s'. Try: set | info | source | gnss | neo6m", cmd);
    return true;
}

bool gps_client_send_raw_at(const char *cmd, char *out, size_t out_size, int timeout_ms) {
#if ENABLE_GPS_GNSS
    return gps_backend_gnss_send_raw_at(cmd, out, out_size, timeout_ms);
#else
    if (out_size > 0) {
        snprintf(out, out_size, "AT passthrough needs the GNSS backend's modem UART "
                                 "(ENABLE_GPS_GNSS=0 in this build) — nothing to send to.");
    }
    return false;
#endif
}
