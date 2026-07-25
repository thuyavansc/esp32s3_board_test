/**
 * gps_backend_neo6m.c — u-blox NEO-6M NMEA GPS backend
 *
 * See gps_backend_neo6m.h for the role this plays in the gps_client.c
 * dispatcher. NMEA parsing logic is ported near-verbatim from
 * esp32_display_taxi_meter's backend/gps_client.c (already tested on an
 * S3 board this session) — the only structural change is that fixes are
 * published via gps_client_set_fix() instead of this file owning the
 * shared state directly.
 *
 * ================================================================
 * NMEA SENTENCE FORMAT (what we parse):
 *
 *   $GPGGA,time,lat,N,lon,E,qual,sats,hdop,alt,M,geoid,M,age,ref*cs
 *     Field 6: 0-2        — Fix quality (0=invalid, 1=GPS, 2=DGPS)
 *     Field 7: nn         — Satellites used
 *     Field 8: d.d        — HDOP
 *     Field 9: d.d        — Altitude (meters, above MSL)
 *
 *   $GPRMC,time,status,lat,N,lon,E,speed,course,date,magvar,varDir,mode*cs
 *     Field 2: A/V        — Status (A=valid, V=void)
 *     Field 7: d.d        — Speed over ground (knots)
 *     Field 8: d.d        — Course over ground (degrees)
 *
 * NMEA latitude/longitude format: ddmm.mmmm (degrees + minutes as one
 * number) — _nmea_to_deg() converts to decimal degrees.
 * ================================================================
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "config.h"
#include "gps_client.h"
#include "gps_backend_neo6m.h"

static const char *TAG = "gps_neo6m";

// ── GPS data (latest fix, published to the dispatcher after each parse) ──
static gps_data_t s_gps_data = {0};
static int        s_total_reads  = 0;
static int        s_valid_reads  = 0;

// ── Command-controlled reading state ──────────────────────────
static bool  s_cmd_active     = false;   // true when user started a read command
static int   s_interval_sec   = 2;       // default: 2 seconds between reads
static int   s_remaining      = -1;      // -1 = infinite, >0 = countdown
static int   s_count_total    = 0;       // total count specified (for logging)
static unsigned long s_last_read_ms = 0;

// ═══════════════════════════════════════════════════════════════
//  NMEA → Decimal Degrees Conversion
// ═══════════════════════════════════════════════════════════════
static double _nmea_to_deg(const char *coord, char dir) {
    if (!coord || strlen(coord) < 4) return 0;
    double raw = atof(coord) / 100.0;
    double deg = floor(raw);
    double min = (raw - deg) * 100.0;
    double val = deg + min / 60.0;
    if (dir == 'S' || dir == 'W') val = -val;
    return val;
}

// ═══════════════════════════════════════════════════════════════
//  Parse $GPGGA sentence — position, altitude, satellites, fix
// ═══════════════════════════════════════════════════════════════
static bool _parse_gga(const char *sentence, gps_data_t *out) {
    if (strncmp(sentence, "$GPGGA", 6) != 0 &&
        strncmp(sentence, "$GNGGA", 6) != 0) return false;

    char tbuf[16] = {0}, lat[16] = {0}, ns = 'N', lon[16] = {0}, ew = 'E';
    int q = 0, sats = 0;
    float hdop = 0, alt = 0;

    int n = sscanf(sentence, "$GPGGA,%15[^,],%15[^,],%c,%15[^,],%c,%d,%d,%f,%f,M",
                   tbuf, lat, &ns, lon, &ew, &q, &sats, &hdop, &alt);
    if (n < 9) {
        // Try GNGGA (multi-constellation)
        n = sscanf(sentence, "$GNGGA,%15[^,],%15[^,],%c,%15[^,],%c,%d,%d,%f,%f,M",
                   tbuf, lat, &ns, lon, &ew, &q, &sats, &hdop, &alt);
    }

    if (q == 0 || strlen(lat) < 4 || strlen(lon) < 4) return false;

    out->lat         = _nmea_to_deg(lat, ns);
    out->lon         = _nmea_to_deg(lon, ew);
    out->alt         = alt;
    out->satellites  = sats;
    out->hdop        = hdop;
    out->fix_quality = q;
    out->has_fix     = (q > 0);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Parse $GPRMC sentence — speed (knots) and course (degrees)
// ═══════════════════════════════════════════════════════════════
static bool _parse_rmc(const char *sentence, gps_data_t *out) {
    if (strncmp(sentence, "$GPRMC", 6) != 0 &&
        strncmp(sentence, "$GNRMC", 6) != 0) return false;

    char status = 'V';
    float speed_knots = 0, course = 0;

    // sentence+6 skips the already-verified 6-char "$GPRMC"/"$GNRMC"
    // prefix (see the strncmp check above) so the same format string
    // works for both variants — $GNRMC is very common on combo
    // GPS+GLONASS receivers, which most NEO-6M modules actually are.
    sscanf(sentence + 6, ",%*[^,],%c,%*[^,],%*c,%*[^,],%*c,%f,%f",
           &status, &speed_knots, &course);

    if (status == 'A') {
        out->speed  = speed_knots * 1.852;   // knots → km/h
        out->course = course;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Log GPS data to serial monitor
// ═══════════════════════════════════════════════════════════════
static void _log_gps_data(const gps_data_t *g) {
    if (g->has_fix) {
        ESP_LOGI(TAG, "──────────────────────────────────────────");
        ESP_LOGI(TAG, "GPS FIX ✓  #%d (total: %d reads, %d valid)",
                 s_valid_reads, s_total_reads, s_valid_reads);
        ESP_LOGI(TAG, "  Lat:     %.6f°", g->lat);
        ESP_LOGI(TAG, "  Lon:     %.6f°", g->lon);
        ESP_LOGI(TAG, "  Alt:     %.1f m", g->alt);
        ESP_LOGI(TAG, "  Speed:   %.1f km/h", g->speed);
        ESP_LOGI(TAG, "  Course:  %.0f°", g->course);
        ESP_LOGI(TAG, "  Sats:    %d", g->satellites);
        ESP_LOGI(TAG, "  HDOP:    %.1f", g->hdop);
        ESP_LOGI(TAG, "  Quality: %d (%s)",
                 g->fix_quality,
                 g->fix_quality == 0 ? "Invalid" :
                 g->fix_quality == 1 ? "GPS Fix" :
                 g->fix_quality == 2 ? "DGPS Fix" : "Unknown");
        ESP_LOGI(TAG, "──────────────────────────────────────────");
    } else {
        ESP_LOGW(TAG, "GPS: NO FIX — waiting for satellites (quality=%d, sats=%d)",
                 g->fix_quality, g->satellites);
    }
}

// ═══════════════════════════════════════════════════════════════
//  GPS READ TASK — continuously reads NMEA sentences from UART
// ═══════════════════════════════════════════════════════════════
static void _gps_read_task(void *arg) {
    char line[128];
    int  pos = 0;

    ESP_LOGI(TAG, "GPS read task STARTED — UART%d @ %d baud",
             GPS_UART_NUM, GPS_BAUD);
    ESP_LOGI(TAG, "NEO-6M may take 1-2 minutes for first fix (cold start)");
    ESP_LOGI(TAG, "Use: gps info | gps once | gps start | gps every <sec> [cnt]");

    while (1) {
        uint8_t c;
        while (uart_read_bytes(GPS_UART_NUM, &c, 1, pdMS_TO_TICKS(20)) > 0) {
            if (c == '\n') {
                line[pos] = '\0';

                gps_data_t tmp = {0};
                if (_parse_gga(line, &tmp)) {
                    s_gps_data.lat         = tmp.lat;
                    s_gps_data.lon         = tmp.lon;
                    s_gps_data.alt         = tmp.alt;
                    s_gps_data.satellites  = tmp.satellites;
                    s_gps_data.hdop        = tmp.hdop;
                    s_gps_data.fix_quality = tmp.fix_quality;
                    s_gps_data.has_fix     = tmp.has_fix;
                }
                _parse_rmc(line, &s_gps_data);

                if (s_gps_data.has_fix) {
                    s_valid_reads++;
                    gps_client_set_fix(&s_gps_data);   // publish to the dispatcher
                }
                s_total_reads++;
                pos = 0;
            } else if (c != '\r' && pos < (int)sizeof(line) - 1) {
                line[pos++] = (char)c;
            }
        }

        if (s_cmd_active) {
            unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - s_last_read_ms >= (unsigned long)(s_interval_sec * 1000)) {
                s_last_read_ms = now;
                _log_gps_data(&s_gps_data);

                if (s_remaining > 0) {
                    s_remaining--;
                    if (s_remaining <= 0) {
                        s_cmd_active = false;
                        ESP_LOGI(TAG, "GPS reading COMPLETE — %d readings done", s_count_total);
                    }
                }
            }
        }

        if (!s_cmd_active) {
            static unsigned long last_status = 0;
            unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - last_status >= 30000) {
                last_status = now;
                if (s_gps_data.has_fix) {
                    ESP_LOGI(TAG, "GPS: fix OK | lat=%.4f lon=%.4f speed=%.1fkm/h sats=%d hdop=%.1f (idle — use 'gps start' for full log)",
                             s_gps_data.lat, s_gps_data.lon, s_gps_data.speed,
                             s_gps_data.satellites, s_gps_data.hdop);
                } else {
                    ESP_LOGI(TAG, "GPS: no fix | reads=%d (idle — waiting for satellites)",
                             s_total_reads);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ═══════════════════════════════════════════════════════════════
//  GPS UART Initialization
// ═══════════════════════════════════════════════════════════════
static void _gps_uart_init(void) {
    uart_config_t uc = {
        .baud_rate  = GPS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &uc));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, GPS_UART_TX, GPS_UART_RX,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "GPS UART init: TX=GPIO%d RX=GPIO%d %d baud ✓",
             GPS_UART_TX, GPS_UART_RX, GPS_BAUD);
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API — Init
// ═══════════════════════════════════════════════════════════════
esp_err_t gps_backend_neo6m_init(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "GPS NEO-6M Module Init");
    ESP_LOGI(TAG, "  UART:       UART%d", GPS_UART_NUM);
    ESP_LOGI(TAG, "  TX Pin:     GPIO%d", GPS_UART_TX);
    ESP_LOGI(TAG, "  RX Pin:     GPIO%d", GPS_UART_RX);
    ESP_LOGI(TAG, "  Baud:       %d", GPS_BAUD);

    _gps_uart_init();

    xTaskCreate(_gps_read_task, "gps_read", 3072, NULL, 2, NULL);

    ESP_LOGI(TAG, "GPS Module — READY ✓");
    ESP_LOGI(TAG, "Commands: gps start | gps stop | gps once | gps every <sec> [cnt] | gps info");
    ESP_LOGI(TAG, "══════════════════════════════════════");
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API — Process backend-specific GPS serial commands
//
//  Command format: gps <subcommand> [args]  (receives the FULL line,
//  including the "gps " prefix — gps_client.c only strips "set"/"info"
//  before delegating here, so this re-parses the same way the original
//  standalone module did.)
// ═══════════════════════════════════════════════════════════════
bool gps_backend_neo6m_process_command(const char *line) {
    if (!line) return false;

    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "gps ", 4) != 0 && strcmp(line, "gps") != 0) return false;

    const char *cmd_start = line + 3;
    while (*cmd_start == ' ') cmd_start++;
    if (*cmd_start == '\0') return false;   // no subcommand — gps_client.c already printed help

    char cmd[16] = {0};
    const char *p = cmd_start;
    int ci = 0;
    while (*p && *p != ' ' && ci < 15) cmd[ci++] = *p++;
    cmd[ci] = '\0';
    while (*p == ' ') p++;

    if (strcmp(cmd, "start") == 0) {
        ESP_LOGI(TAG, "▶ GPS START — continuous logging (every 2s)");
        s_cmd_active    = true;
        s_interval_sec  = 2;
        s_remaining     = -1;  // infinite
        s_count_total   = 0;
        s_last_read_ms  = 0;
        return true;
    }

    if (strcmp(cmd, "stop") == 0) {
        ESP_LOGI(TAG, "▶ GPS STOP");
        if (s_cmd_active) {
            ESP_LOGI(TAG, "  Stopped after %d readings (total: %d valid / %d reads)",
                     s_count_total - (s_remaining > 0 ? s_remaining : 0),
                     s_valid_reads, s_total_reads);
        }
        s_cmd_active = false;
        return true;
    }

    if (strcmp(cmd, "once") == 0) {
        ESP_LOGI(TAG, "▶ GPS ONCE — single reading");
        _log_gps_data(&s_gps_data);
        ESP_LOGI(TAG, "GPS once — DONE");
        return true;
    }

    if (strcmp(cmd, "every") == 0) {
        int interval = 2;   // default
        int count    = -1;  // default: infinite

        while (*p == ' ') p++;
        if (*p && isdigit((unsigned char)*p)) {
            interval = (int)strtol(p, (char **)&p, 10);
            if (interval < 1) interval = 1;
            if (interval > 3600) interval = 3600;
        }

        while (*p == ' ') p++;
        if (*p && isdigit((unsigned char)*p)) {
            count = (int)strtol(p, NULL, 10);
            if (count < 1) count = 1;
            if (count > 10000) count = 10000;
        }

        ESP_LOGI(TAG, "▶ GPS EVERY %ds %s",
                 interval, count > 0 ? "" : "(until stopped)");
        if (count > 0) {
            ESP_LOGI(TAG, "  Will run %d time(s)", count);
        }
        s_cmd_active    = true;
        s_interval_sec  = interval;
        s_remaining     = count;
        s_count_total   = count;
        s_last_read_ms  = 0;
        return true;
    }

    // "info" is handled by gps_client.c itself before delegating here —
    // reaching this point with an unrecognized subcommand is a genuine
    // unknown command.
    ESP_LOGW(TAG, "Unknown GPS command: '%s'. Try: gps start | stop | once | every | set | info", cmd);
    return true;
}
