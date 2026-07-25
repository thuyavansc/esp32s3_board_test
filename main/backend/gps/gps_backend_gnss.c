/**
 * gps_backend_gnss.c — A7670E built-in GNSS backend (AT commands over UART1)
 *
 * See gps_backend_gnss.h for the full design + sourcing. This module
 * owns the modem's AT-command bring-up sequence for GNSS specifically —
 * it does NOT touch the modem's cellular-data/PPP path (a completely
 * separate interface, untouched by this project) — see doc 140 for why
 * those are independent.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "config.h"
#include "gps_client.h"
#include "gps_nmea.h"
#include "gps_backend_gnss.h"

static const char *TAG = "gps_gnss";

static gps_data_t s_gnss_data = {0};
static bool s_enabled  = true;   // runtime on/off (independent of ENABLE_GPS_GNSS compile flag)
static bool s_at_ready = false;  // modem has answered "AT" at least once — bring-up completed
static int  s_total_reads = 0;
static int  s_valid_reads = 0;

// ═══════════════════════════════════════════════════════════════
//  Blocking AT-command helper — setup phase only, before continuous
//  NMEA streaming starts. Same "read until OK/ERROR or timeout" pattern
//  already proven in this repo (esp32s3_4g_hotspot/gps_manager.c's own
//  _at_send()).
// ═══════════════════════════════════════════════════════════════
static int _at_send(const char *cmd, char *resp, size_t resp_len, int timeout_ms) {
    uart_flush_input(GNSS_UART_NUM);
    uart_write_bytes(GNSS_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(GNSS_UART_NUM, "\r\n", 2);

    size_t pos = 0;
    resp[0] = '\0';
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        uint8_t c;
        if (uart_read_bytes(GNSS_UART_NUM, &c, 1, pdMS_TO_TICKS(50)) > 0) {
            if (pos < resp_len - 1) resp[pos++] = (char)c;
            resp[pos] = '\0';
            if (strstr(resp, "OK\r\n") || strstr(resp, "ERROR\r\n") || strstr(resp, "ERROR:")) break;
        }
    }
    return (int)pos;
}

static bool _at_ok(const char *cmd, int timeout_ms) {
    char resp[160];
    _at_send(cmd, resp, sizeof(resp), timeout_ms);
    bool ok = strstr(resp, "OK") != NULL;
    ESP_LOGI(TAG, "  %s -> %s", cmd, ok ? "OK" : "no OK (see raw response below)");
    if (!ok) ESP_LOGW(TAG, "    raw: %.100s", resp);
    return ok;
}

static void _uart_init(void) {
    uart_config_t cfg = {
        .baud_rate  = GNSS_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GNSS_UART_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GNSS_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GNSS_UART_NUM, GNSS_UART_TX, GNSS_UART_RX,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "GNSS UART init: UART%d TX=GPIO%d RX=GPIO%d %d baud",
             GNSS_UART_NUM, GNSS_UART_TX, GNSS_UART_RX, GNSS_UART_BAUD);
}

#if ENABLE_GNSS_PWRKEY
// Not needed on the confirmed-working ESP-IDF reference sample for this
// exact chip (relies on the board's own 4G DIP switch instead) — kept
// here, OFF by default, in case your specific board revision needs it
// (doc 139 §9 open item 2 — verify before enabling).
static void _pulse_pwrkey(void) {
    gpio_set_direction(GNSS_PWRKEY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(GNSS_PWRKEY_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GNSS_PWRKEY_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "PWRKEY pulsed (GPIO%d)", GNSS_PWRKEY_GPIO);
}
#endif

// ═══════════════════════════════════════════════════════════════
//  A-GPS / SUPL setup — staged behind ENABLE_AGPS (config.h), OFF by
//  default until a data-enabled SIM is inserted (doc 140). Checks SIM +
//  network registration FIRST and skips cleanly — falling back to plain
//  GNSS — rather than sending SUPL triggers that would just fail/timeout
//  with nothing to show for it.
// ═══════════════════════════════════════════════════════════════
#if ENABLE_AGPS
static bool _agps_preconditions_ok(void) {
    char resp[128];

    _at_send("AT+CPIN?", resp, sizeof(resp), 2000);
    if (!strstr(resp, "+CPIN: READY")) {
        ESP_LOGW(TAG, "A-GPS enabled but SIM not ready (AT+CPIN? -> %.80s)", resp);
        ESP_LOGW(TAG, "  Falling back to plain (non-assisted) GNSS.");
        return false;
    }

    _at_send("AT+CREG?", resp, sizeof(resp), 2000);
    if (!strstr(resp, "+CREG: 0,1") && !strstr(resp, "+CREG: 0,5")) {
        ESP_LOGW(TAG, "A-GPS enabled but not registered on a cellular network (AT+CREG? -> %.80s)", resp);
        ESP_LOGW(TAG, "  Falling back to plain (non-assisted) GNSS.");
        return false;
    }

    ESP_LOGI(TAG, "A-GPS preconditions OK — SIM ready, network registered.");
    return true;
}

static void _agps_setup(void) {
    if (!_agps_preconditions_ok()) return;

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+SUPLSERVER=\"%s\",%d", AGPS_SUPL_SERVER, AGPS_SUPL_PORT);
    _at_ok(cmd, 2000);

    if (AGPS_ENABLE_XTRA)     _at_ok("AT+CGNSSCMD=10,1", 2000);   // XTRA — predicted ephemeris
    if (AGPS_ENABLE_SUPL)     _at_ok("AT+CGNSSCMD=20,1", 2000);   // standard OMA SUPL A-GPS
    if (AGPS_ENABLE_HOTSTILL) _at_ok("AT+CGNSSCMD=30,1", 2000);   // fast re-acquisition

    ESP_LOGI(TAG, "A-GPS configured (%s:%d) — assist data downloads over the MODEM'S OWN",
             AGPS_SUPL_SERVER, AGPS_SUPL_PORT);
    ESP_LOGI(TAG, "  cellular connection (not WiFi — see doc 140). Cold fix should now be");
    ESP_LOGI(TAG, "  much faster (~1-5s instead of 35-90s) once assist data arrives.");
}
#else
static void _agps_setup(void) {
    ESP_LOGI(TAG, "A-GPS disabled (ENABLE_AGPS=0 in config.h) — plain GNSS only.");
    ESP_LOGI(TAG, "  Insert a data-enabled SIM, set ENABLE_AGPS=1, rebuild — see doc 140.");
}
#endif

// ═══════════════════════════════════════════════════════════════
//  One-time bring-up: AT handshake, (A-GPS), power GNSS, start NMEA,
//  route it to this UART.
// ═══════════════════════════════════════════════════════════════
static bool _bring_up_gnss(void) {
    ESP_LOGI(TAG, "AT handshake...");
    int retries = 0;
    while (!_at_ok("AT", 2000)) {
        retries++;
        if (retries % 5 == 0) {
            ESP_LOGW(TAG, "Modem not responding to AT after %d attempts — check the board's", retries);
            ESP_LOGW(TAG, "  4G DIP switch is ON, power, and UART1 wiring (GPIO18/17).");
        }
        if (retries >= 30) {
            ESP_LOGE(TAG, "Modem never responded to AT after %d attempts — giving up GNSS bring-up.", retries);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Modem responding \xE2\x9C\x93");

    _agps_setup();

    _at_ok("AT+CGNSSPWR=1", 5000);
    vTaskDelay(pdMS_TO_TICKS(300));
    _at_ok("AT+CGNSSTST=1", 5000);
    vTaskDelay(pdMS_TO_TICKS(300));

    char portcmd[48];
    snprintf(portcmd, sizeof(portcmd), "AT+CGNSSPORTSWITCH=%s", GNSS_CGNSSPORTSWITCH_ARGS);
    _at_ok(portcmd, 5000);

    ESP_LOGI(TAG, "GNSS bring-up complete — NMEA streaming should begin shortly (cold start:");
    ESP_LOGI(TAG, "  35-90s typical outdoors, faster if A-GPS assist data was downloaded).");
    return true;
}

static void _resume_gnss(void) {
    _at_ok("AT+CGNSSPWR=1", 5000);
    vTaskDelay(pdMS_TO_TICKS(300));
    _at_ok("AT+CGNSSTST=1", 5000);
    vTaskDelay(pdMS_TO_TICKS(300));
    char portcmd[48];
    snprintf(portcmd, sizeof(portcmd), "AT+CGNSSPORTSWITCH=%s", GNSS_CGNSSPORTSWITCH_ARGS);
    _at_ok(portcmd, 5000);
}

static void _power_down_gnss(void) {
    _at_ok("AT+CGNSSPWR=0", 3000);
}

// ═══════════════════════════════════════════════════════════════
//  Continuous NMEA read task — same line-assembly pattern as the NEO-6M
//  backend (backend/gps/gps_backend_neo6m.c), just reading UART1 instead
//  of UART2, through the same shared gps_nmea.c parser.
// ═══════════════════════════════════════════════════════════════
static void _gnss_read_task(void *arg) {
    (void)arg;
    char line[128];
    int  pos = 0;

    if (!_bring_up_gnss()) {
        ESP_LOGE(TAG, "GNSS bring-up failed — task exiting. Backend will report NO FIX until");
        ESP_LOGE(TAG, "  the board is reset (check modem power/wiring, then reflash/reset).");
        vTaskDelete(NULL);
        return;
    }
    s_at_ready = true;

    while (1) {
        uint8_t c;
        while (uart_read_bytes(GNSS_UART_NUM, &c, 1, pdMS_TO_TICKS(20)) > 0) {
            if (c == '\n') {
                line[pos] = '\0';

                if (s_enabled) {
                    gps_data_t tmp = {0};
                    if (gps_nmea_parse_gga(line, &tmp)) {
                        s_gnss_data.lat         = tmp.lat;
                        s_gnss_data.lon         = tmp.lon;
                        s_gnss_data.alt         = tmp.alt;
                        s_gnss_data.satellites  = tmp.satellites;
                        s_gnss_data.hdop        = tmp.hdop;
                        s_gnss_data.fix_quality = tmp.fix_quality;
                        s_gnss_data.has_fix     = tmp.has_fix;
                    }
                    gps_nmea_parse_rmc(line, &s_gnss_data);

                    if (s_gnss_data.has_fix) {
                        s_valid_reads++;
                        gps_client_publish_gnss_fix(&s_gnss_data);
                    }
                    s_total_reads++;
                }
                pos = 0;
            } else if (c != '\r' && pos < (int)sizeof(line) - 1) {
                line[pos++] = (char)c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════
esp_err_t gps_backend_gnss_init(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "GNSS (A7670E) Init");
    ESP_LOGI(TAG, "  A-GPS: %s", ENABLE_AGPS ? "ENABLED in config.h" : "disabled (no SIM yet — see doc 140)");
    ESP_LOGI(TAG, "══════════════════════════════════════");

    _uart_init();
#if ENABLE_GNSS_PWRKEY
    _pulse_pwrkey();
#endif
    xTaskCreate(_gnss_read_task, "gnss_read", 4096, NULL, 3, NULL);
    return ESP_OK;
}

void gps_backend_gnss_set_enabled(bool enable) {
    if (enable == s_enabled) return;
    s_enabled = enable;

    if (!enable) {
        _power_down_gnss();
        ESP_LOGI(TAG, "GNSS backend DISABLED — AT+CGNSSPWR=0 sent (engine actually powered down)");
    } else {
        ESP_LOGI(TAG, "GNSS backend RE-ENABLED — powering GNSS back on");
        _resume_gnss();
    }
}

bool gps_backend_gnss_is_enabled(void) { return s_enabled; }

bool gps_backend_gnss_process_command(const char *args) {
    if (strcmp(args, "on") == 0)  { gps_backend_gnss_set_enabled(true);  return true; }
    if (strcmp(args, "off") == 0) { gps_backend_gnss_set_enabled(false); return true; }

    if (strcmp(args, "info") == 0) {
        ESP_LOGI(TAG, "══════════════════════════════════════");
        ESP_LOGI(TAG, "GNSS (A7670E) STATUS");
        ESP_LOGI(TAG, "──────────────────────────────────────");
        ESP_LOGI(TAG, "  UART:        UART%d TX=GPIO%d RX=GPIO%d @ %d baud",
                 GNSS_UART_NUM, GNSS_UART_TX, GNSS_UART_RX, GNSS_UART_BAUD);
        ESP_LOGI(TAG, "  Bring-up:    %s", s_at_ready ? "complete" : "in progress / modem not responding yet");
        ESP_LOGI(TAG, "  Enabled:     %s", s_enabled ? "yes" : "no (AT+CGNSSPWR=0 sent)");
        ESP_LOGI(TAG, "  A-GPS:       %s", ENABLE_AGPS ? "enabled in config.h" : "disabled in config.h (no SIM yet)");
        ESP_LOGI(TAG, "  Reads:       %d total | %d valid fixes", s_total_reads, s_valid_reads);
        ESP_LOGI(TAG, "  Fix:         %s", s_gnss_data.has_fix ? "YES \xE2\x9C\x93" : "NO");
        if (s_gnss_data.has_fix) {
            ESP_LOGI(TAG, "  Latitude:    %.6f\xC2\xB0", s_gnss_data.lat);
            ESP_LOGI(TAG, "  Longitude:   %.6f\xC2\xB0", s_gnss_data.lon);
            ESP_LOGI(TAG, "  Speed:       %.1f km/h", s_gnss_data.speed);
            ESP_LOGI(TAG, "  Satellites:  %d | HDOP: %.1f", s_gnss_data.satellites, s_gnss_data.hdop);
        }
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return true;
    }

    if (strcmp(args, "agps") == 0) {
        ESP_LOGI(TAG, "Manually re-triggering A-GPS setup...");
        _agps_setup();
        return true;
    }

    ESP_LOGW(TAG, "Unknown 'gps gnss' command: '%s'. Try: on | off | info | agps", args);
    return true;
}
