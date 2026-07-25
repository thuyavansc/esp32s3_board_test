/**
 * gps_client.c — GPS dispatcher (see gps_client.h for the full design).
 *
 * Owns the one shared gps_data_t every backend publishes into and every
 * consumer (fare_calc.c) reads from. Routes init()/process_command() to
 * whichever backend GPS_SOURCE (config.h) selects; "gps set ..." (the
 * PC-GUI injection path) is handled directly here, unconditionally, so
 * it always works regardless of which backend is "driving".
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "config.h"
#include "gps_client.h"

#if GPS_SOURCE == GPS_SRC_NEO6M
#include "driver/uart.h"    // UART_NUM_2 (GPS_UART_NUM) referenced in the log lines below
#include "gps_backend_neo6m.h"
#endif

static const char *TAG = "gps";

static gps_data_t s_latest  = {0};
static bool       s_has_fix = false;
static bool       s_running = false;

void gps_client_set_fix(const gps_data_t *fix) {
    if (!fix) return;
    s_latest  = *fix;
    s_has_fix = fix->has_fix;
}

const gps_data_t *gps_client_get_latest(void) {
    return s_has_fix ? &s_latest : NULL;
}

const char *gps_client_get_status(void) {
    if (!s_running) return "DISABLED";
    if (!s_has_fix) return "NO FIX";
    return "FIX OK";
}

bool gps_client_is_running(void) {
    return s_running;
}

esp_err_t gps_client_init(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "GPS Client Init — GPS_SOURCE=%d", GPS_SOURCE);

#if GPS_SOURCE == GPS_SRC_NEO6M
    ESP_LOGI(TAG, "  Backend: NEO-6M (NMEA over UART%d, TX=GPIO%d RX=GPIO%d)",
             GPS_UART_NUM, GPS_UART_TX, GPS_UART_RX);
    esp_err_t err = gps_backend_neo6m_init();
    s_running = (err == ESP_OK);
#elif GPS_SOURCE == GPS_SRC_GNSS
    // NOT YET IMPLEMENTED — see gps_client.h's file header and
    // docs/TestFunctionalities/esp32s3_board/
    // 136_2026-07-25_gpio_uart_conflict_analysis_and_board_identity_question.md
    // §5. The A7670E's AT-command UART (GPIO 18 TX / 17 RX) is confirmed
    // reserved/working — the backend itself (AT+CGNSSPWR/CGPSINFO +
    // A-GPS/SUPL setup, ported from esp32s3_4g_hotspot/gps_manager.c)
    // is intentionally not wired in here yet, pending confirmation of
    // which physical board this firmware runs on. Falls back to "no
    // fix" rather than pretending to talk to hardware that may not be
    // attached — use "gps set ..." (always available) to inject fixes
    // for testing until this backend is implemented.
    ESP_LOGW(TAG, "  Backend: GNSS/A-GPS (A7670E) — NOT YET IMPLEMENTED.");
    ESP_LOGW(TAG, "  Use 'gps set <lat> <lon> <speed>' to inject fixes for testing,");
    ESP_LOGW(TAG, "  or switch GPS_SOURCE to GPS_SRC_NEO6M in config.h for a real module.");
    s_running = true;   // the injection path still works
#else
    ESP_LOGI(TAG, "  Backend: INJECT (serial 'gps set ...' from the PC GUI — no GPS hardware)");
    s_running = true;
#endif

    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  "gps set <lat> <lon> <speed_kmh> [hdop] [sats]" — always available,
//  regardless of GPS_SOURCE. Publishes a fix exactly as if a real
//  receiver had produced it — fare_calc.c can't tell the difference.
// ═══════════════════════════════════════════════════════════════
static bool _cmd_set(const char *args) {
    double lat = 0, lon = 0, speed = 0, hdop = 1.0;
    int sats = 6;
    int n = sscanf(args, "%lf %lf %lf %lf %d", &lat, &lon, &speed, &hdop, &sats);
    if (n < 3) {
        printf("Usage: gps set <lat> <lon> <speed_kmh> [hdop] [sats]\n");
        return true;
    }

    gps_data_t fix = {
        .lat = lat, .lon = lon, .speed = speed, .hdop = hdop,
        .satellites = sats, .fix_quality = 1, .has_fix = true,
        .alt = 0, .course = 0,
    };
    gps_client_set_fix(&fix);
    ESP_LOGI(TAG, "▶ GPS INJECTED — lat=%.6f lon=%.6f speed=%.1fkm/h hdop=%.1f sats=%d",
             lat, lon, speed, hdop, sats);
    return true;
}

static void _cmd_info(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "GPS STATUS");
    ESP_LOGI(TAG, "──────────────────────────────────────");
#if GPS_SOURCE == GPS_SRC_NEO6M
    ESP_LOGI(TAG, "  Backend:      NEO-6M (UART%d, TX=GPIO%d RX=GPIO%d)", GPS_UART_NUM, GPS_UART_TX, GPS_UART_RX);
#elif GPS_SOURCE == GPS_SRC_GNSS
    ESP_LOGI(TAG, "  Backend:      GNSS/A-GPS (A7670E) — NOT YET IMPLEMENTED, injection-only");
#else
    ESP_LOGI(TAG, "  Backend:      INJECT (serial 'gps set ...')");
#endif
    ESP_LOGI(TAG, "  Status:       %s", gps_client_get_status());
    if (s_has_fix) {
        ESP_LOGI(TAG, "  Latitude:     %.6f°", s_latest.lat);
        ESP_LOGI(TAG, "  Longitude:    %.6f°", s_latest.lon);
        ESP_LOGI(TAG, "  Speed:        %.1f km/h", s_latest.speed);
        ESP_LOGI(TAG, "  HDOP:         %.1f", s_latest.hdop);
        ESP_LOGI(TAG, "  Satellites:   %d", s_latest.satellites);
        ESP_LOGI(TAG, "  Fix quality:  %d", s_latest.fix_quality);
    }
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

bool gps_client_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "gps ", 4) != 0 && strcmp(line, "gps") != 0) return false;

    const char *cmd_start = line + 3;
    while (*cmd_start == ' ') cmd_start++;

    if (*cmd_start == '\0') {
        ESP_LOGI(TAG, "GPS Commands: gps set <lat> <lon> <speed> [hdop] [sats] | gps info"
#if GPS_SOURCE == GPS_SRC_NEO6M
                 " | gps start | gps stop | gps once | gps every <sec> [cnt]"
#endif
        );
        return true;
    }

    char cmd[16] = {0};
    const char *p = cmd_start;
    int ci = 0;
    while (*p && *p != ' ' && ci < 15) cmd[ci++] = *p++;
    cmd[ci] = '\0';
    while (*p == ' ') p++;

    if (strcmp(cmd, "set") == 0)  return _cmd_set(p);
    if (strcmp(cmd, "info") == 0) { _cmd_info(); return true; }

#if GPS_SOURCE == GPS_SRC_NEO6M
    // Backend-specific extras (start/stop/once/every) — only meaningful
    // when a real NEO-6M module is actually feeding this dispatcher.
    return gps_backend_neo6m_process_command(line);
#else
    ESP_LOGW(TAG, "Unknown GPS command: '%s'. Try: gps set | gps info", cmd);
    return true;
#endif
}
