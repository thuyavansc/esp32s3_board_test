/**
 * gps_backend_neo6m.c — u-blox NEO-6M NMEA GPS backend
 *
 * See gps_backend_neo6m.h for the role this plays in the gps_client.c
 * dispatcher. NMEA parsing goes through the shared gps_nmea.c parser
 * (also used by gps_backend_gnss.c — both emit identical NMEA) — this
 * file owns only the UART/task/command-state machinery, ported
 * near-verbatim from esp32_display_taxi_meter's backend/gps_client.c
 * (already tested on an S3 board this session).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "config.h"
#include "gps_client.h"
#include "gps_nmea.h"
#include "gps_backend_neo6m.h"

static const char *TAG = "gps_neo6m";

// ── GPS data (latest fix, published to the dispatcher after each parse) ──
static gps_data_t s_gps_data = {0};
static bool       s_enabled     = true;   // runtime on/off (independent of ENABLE_GPS_NEO6M compile flag)
static int        s_total_reads = 0;
static int        s_valid_reads = 0;

// ── Command-controlled reading state ──────────────────────────
static bool  s_cmd_active     = false;   // true when user started a read command
static int   s_interval_sec   = 2;       // default: 2 seconds between reads
static int   s_remaining      = -1;      // -1 = infinite, >0 = countdown
static int   s_count_total    = 0;       // total count specified (for logging)
static unsigned long s_last_read_ms = 0;

// Doc 142 §4.2 — lets a periodic status log say whether ITS OWN backend
// is the one actually feeding fare_calc right now, without having to
// separately run "gps info". Same helper duplicated (not shared) in
// gps_backend_gnss.c — trivial logic, not worth a new coupling.
static const char *_active_tag(gps_source_t mine) {
    return (gps_client_get_active_source() == mine) ? "[ACTIVE]" : "[not active]";
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
             NEO6M_UART_NUM, NEO6M_UART_BAUD);
    ESP_LOGI(TAG, "NEO-6M may take 1-2 minutes for first fix (cold start)");
    ESP_LOGI(TAG, "Use: gps neo6m info | once | start | every <sec> [cnt]");

    while (1) {
        uint8_t c;
        while (uart_read_bytes(NEO6M_UART_NUM, &c, 1, pdMS_TO_TICKS(20)) > 0) {
            if (c == '\n') {
                line[pos] = '\0';

                if (s_enabled) {
                    gps_data_t tmp = {0};
                    if (gps_nmea_parse_gga(line, &tmp)) {
                        s_gps_data.lat         = tmp.lat;
                        s_gps_data.lon         = tmp.lon;
                        s_gps_data.alt         = tmp.alt;
                        s_gps_data.satellites  = tmp.satellites;
                        s_gps_data.hdop        = tmp.hdop;
                        s_gps_data.fix_quality = tmp.fix_quality;
                        s_gps_data.has_fix     = tmp.has_fix;
                    }
                    gps_nmea_parse_rmc(line, &s_gps_data);

                    if (s_gps_data.has_fix) {
                        s_valid_reads++;
                        gps_client_publish_neo6m_fix(&s_gps_data);   // publish to the dispatcher
                    }
                    s_total_reads++;
                }
                // when disabled: bytes are still drained above (avoids UART
                // buffer overflow) but simply not parsed/published — matches
                // gps_backend_gnss.c's same "drain but discard" behavior.
                pos = 0;
            } else if (c != '\r' && pos < (int)sizeof(line) - 1) {
                line[pos++] = (char)c;
            }
        }

        if (s_enabled && s_cmd_active) {
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

        if (s_enabled && !s_cmd_active) {
            static unsigned long last_status = 0;
            unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - last_status >= 30000) {
                last_status = now;
                const char *tag = _active_tag(GPS_SRC_NEO6M);
                if (s_gps_data.has_fix) {
                    ESP_LOGI(TAG, "GPS: fix OK | lat=%.4f lon=%.4f speed=%.1fkm/h sats=%d hdop=%.1f %s (idle — use 'gps neo6m start' for full log)",
                             s_gps_data.lat, s_gps_data.lon, s_gps_data.speed,
                             s_gps_data.satellites, s_gps_data.hdop, tag);
                } else {
                    ESP_LOGI(TAG, "GPS: no fix | reads=%d %s (idle — waiting for satellites)",
                             s_total_reads, tag);
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
        .baud_rate  = NEO6M_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(NEO6M_UART_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(NEO6M_UART_NUM, &uc));
    ESP_ERROR_CHECK(uart_set_pin(NEO6M_UART_NUM, NEO6M_UART_TX, NEO6M_UART_RX,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "GPS UART init: TX=GPIO%d RX=GPIO%d %d baud ✓",
             NEO6M_UART_TX, NEO6M_UART_RX, NEO6M_UART_BAUD);
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API — Init
// ═══════════════════════════════════════════════════════════════
esp_err_t gps_backend_neo6m_init(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "GPS NEO-6M Module Init");
    ESP_LOGI(TAG, "  UART:       UART%d", NEO6M_UART_NUM);
    ESP_LOGI(TAG, "  TX Pin:     GPIO%d", NEO6M_UART_TX);
    ESP_LOGI(TAG, "  RX Pin:     GPIO%d", NEO6M_UART_RX);
    ESP_LOGI(TAG, "  Baud:       %d", NEO6M_UART_BAUD);

    _gps_uart_init();

    xTaskCreate(_gps_read_task, "gps_read", 3072, NULL, 2, NULL);

    ESP_LOGI(TAG, "GPS Module — READY ✓");
    ESP_LOGI(TAG, "Commands: gps neo6m on|off|info|start|stop|once|every <sec> [cnt]");
    ESP_LOGI(TAG, "══════════════════════════════════════");
    return ESP_OK;
}

void gps_backend_neo6m_set_enabled(bool enable) {
    if (enable == s_enabled) return;
    s_enabled = enable;
    ESP_LOGI(TAG, "NEO-6M backend %s", enable ? "RE-ENABLED" : "DISABLED (module stays physically powered — no AT power-down exists for a dumb NMEA module; the UART is still drained, just not parsed/published)");
}

bool gps_backend_neo6m_is_enabled(void) { return s_enabled; }

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API — Process backend-specific GPS serial commands
//
//  args is whatever followed "gps neo6m " (the prefix already stripped
//  by gps_client.c) — e.g. "start", "every 5 20", "info".
// ═══════════════════════════════════════════════════════════════
bool gps_backend_neo6m_process_command(const char *args) {
    if (!args) return false;

    char cmd[16] = {0};
    const char *p = args;
    int ci = 0;
    while (*p && *p != ' ' && ci < 15) cmd[ci++] = *p++;
    cmd[ci] = '\0';
    while (*p == ' ') p++;

    if (strcmp(cmd, "on") == 0)  { gps_backend_neo6m_set_enabled(true);  return true; }
    if (strcmp(cmd, "off") == 0) { gps_backend_neo6m_set_enabled(false); return true; }

    if (strcmp(cmd, "info") == 0) {
        ESP_LOGI(TAG, "══════════════════════════════════════");
        ESP_LOGI(TAG, "GPS NEO-6M STATUS");
        ESP_LOGI(TAG, "──────────────────────────────────────");
        ESP_LOGI(TAG, "  UART:        UART%d TX=GPIO%d RX=GPIO%d @ %d baud",
                 NEO6M_UART_NUM, NEO6M_UART_TX, NEO6M_UART_RX, NEO6M_UART_BAUD);
        ESP_LOGI(TAG, "  Enabled:      %s", s_enabled ? "yes" : "no");
        ESP_LOGI(TAG, "  Total reads:  %d", s_total_reads);
        ESP_LOGI(TAG, "  Valid fixes:  %d", s_valid_reads);
        ESP_LOGI(TAG, "  Fix:          %s", s_gps_data.has_fix ? "YES ✓" : "NO ✗");
        if (s_gps_data.has_fix) {
            ESP_LOGI(TAG, "  Latitude:     %.6f°", s_gps_data.lat);
            ESP_LOGI(TAG, "  Longitude:    %.6f°", s_gps_data.lon);
            ESP_LOGI(TAG, "  Altitude:     %.1f m", s_gps_data.alt);
            ESP_LOGI(TAG, "  Speed:        %.1f km/h", s_gps_data.speed);
            ESP_LOGI(TAG, "  Course:       %.0f°", s_gps_data.course);
            ESP_LOGI(TAG, "  Satellites:   %d", s_gps_data.satellites);
            ESP_LOGI(TAG, "  HDOP:         %.1f", s_gps_data.hdop);
        }
        ESP_LOGI(TAG, "  Cmd state:    %s", s_cmd_active ? "ACTIVE" : "idle");
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return true;
    }

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

    ESP_LOGW(TAG, "Unknown 'gps neo6m' command: '%s'. Try: on | off | info | start | stop | once | every", cmd);
    return true;
}
