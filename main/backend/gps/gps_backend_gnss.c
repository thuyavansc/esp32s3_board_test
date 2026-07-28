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
#include "freertos/semphr.h"
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

// Guards every access to UART1 — shared between this module's own AT
// traffic (_at_send()/_at_ok(), used by bring-up/on/off/agps, all
// callable from serial_cmd_task via gps_backend_gnss_set_enabled()/
// gps_backend_gnss_process_command()) and TWO other things that must
// never interleave with it on the same wire: the continuous NMEA-read
// loop in _gnss_read_task() (a different task, reading byte-by-byte),
// and gps_backend_gnss_send_raw_at() (the AT passthrough, reached from
// the serial command reader — also a different task). Without this,
// two tasks calling uart_read_bytes() on the same port at once would
// each get an unpredictable, interleaved slice of the incoming bytes.
static SemaphoreHandle_t s_uart_mutex;

// Phase 2 (doc 155/159) — SMS URC hook, see gps_backend_gnss.h's own
// comment for the full contract (handler must be non-blocking, must
// never re-take s_uart_mutex).
static gnss_urc_handler_t s_urc_handler = NULL;

// ═══════════════════════════════════════════════════════════════
//  Blocking AT-command helper — setup phase only, before continuous
//  NMEA streaming starts. Same "read until OK/ERROR or timeout" pattern
//  already proven in this repo (esp32s3_4g_hotspot/gps_manager.c's own
//  _at_send()). Mutex-protected — see s_uart_mutex above.
// ═══════════════════════════════════════════════════════════════
static int _at_send(const char *cmd, char *resp, size_t resp_len, int timeout_ms) {
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        resp[0] = '\0';
        ESP_LOGW(TAG, "  UART1 busy (AT passthrough in progress?) — '%s' skipped", cmd);
        return 0;
    }

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
    xSemaphoreGive(s_uart_mutex);
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

// Doc 142 §4.2 — lets a periodic status log say whether ITS OWN backend
// is the one actually feeding fare_calc right now, without having to
// separately run "gps info". Same helper duplicated (not shared) in
// gps_backend_neo6m.c — trivial logic, not worth a new coupling.
static const char *_active_tag(gps_source_t mine) {
    return (gps_client_get_active_source() == mine) ? "[ACTIVE]" : "[not active]";
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
        // Non-blocking take: if an AT passthrough command currently owns
        // the UART, just skip this iteration rather than fight it for
        // bytes — incoming NMEA bytes sit safely in the UART driver's own
        // ring buffer (1024 bytes, installed in _uart_init()) for the
        // brief window a passthrough transaction takes, so nothing is
        // lost, just read a little later.
        if (xSemaphoreTake(s_uart_mutex, 0) == pdTRUE) {
            uint8_t c;
            while (uart_read_bytes(GNSS_UART_NUM, &c, 1, pdMS_TO_TICKS(20)) > 0) {
                if (c == '\n') {
                    line[pos] = '\0';

                    // Phase 2 (doc 155/159) — SMS URC hook. Unconditional
                    // (NOT gated on s_enabled — that flag only controls the
                    // GNSS engine's own power state, unrelated to whether
                    // the modem can receive SMS). Any non-NMEA, non-empty
                    // line goes to the registered handler; NMEA sentences
                    // ('$'-prefixed) are left to the parsing below exactly
                    // as before — this line is the ONLY change to the
                    // existing NMEA read path.
                    if (line[0] != '\0' && line[0] != '$' && s_urc_handler) {
                        s_urc_handler(line);
                    }

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
            xSemaphoreGive(s_uart_mutex);
        }

        // Doc 142 §4.1 — periodic idle heartbeat, mirroring NEO-6M's own
        // (gps_backend_neo6m.c's _gps_read_task). Before this, GNSS never
        // logged anything after the one-time bring-up sequence unless you
        // ran "gps gnss info" yourself — from the serial monitor it looked
        // silent/dead even though it was running fine. Gated on the same
        // real s_enabled flag "gps gnss off" already sets (AT+CGNSSPWR=0),
        // so this never claims activity that isn't actually happening.
        if (s_enabled) {
            static unsigned long last_status = 0;
            unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - last_status >= 30000) {
                last_status = now;
                const char *tag = _active_tag(GPS_SRC_GNSS);
                if (s_gnss_data.has_fix) {
                    ESP_LOGI(TAG, "GNSS: fix OK | lat=%.4f lon=%.4f speed=%.1fkm/h sats=%d hdop=%.1f %s (idle — use 'gps gnss info' for full status)",
                             s_gnss_data.lat, s_gnss_data.lon, s_gnss_data.speed,
                             s_gnss_data.satellites, s_gnss_data.hdop, tag);
                } else {
                    ESP_LOGI(TAG, "GNSS: no fix | reads=%d %s (idle — waiting for satellites)",
                             s_total_reads, tag);
                }
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

    s_uart_mutex = xSemaphoreCreateMutex();
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

// ═══════════════════════════════════════════════════════════════
//  Raw AT passthrough — see gps_backend_gnss.h for the full contract.
//  Same "read until OK/ERROR or timeout" shape as _at_send() above, but
//  line-by-line instead of accumulate-then-scan, so NMEA sentences
//  ('$'-prefixed lines — the GNSS engine can interleave these with AT
//  responses on this same UART once streaming has started) can be
//  filtered out of the response instead of corrupting it.
// ═══════════════════════════════════════════════════════════════
bool gps_backend_gnss_send_raw_at(const char *cmd, char *out, size_t out_size, int timeout_ms) {
    if (out_size > 0) out[0] = '\0';

    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        snprintf(out, out_size, "(AT passthrough busy — GNSS backend mid-transaction, try again)");
        return false;
    }

    uart_flush_input(GNSS_UART_NUM);
    uart_write_bytes(GNSS_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(GNSS_UART_NUM, "\r\n", 2);

    char   line[160];
    int    lpos = 0;
    size_t opos = 0;
    bool   terminal_seen = false;
    TickType_t start = xTaskGetTickCount();

    while (!terminal_seen && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        uint8_t c;
        if (uart_read_bytes(GNSS_UART_NUM, &c, 1, pdMS_TO_TICKS(50)) <= 0) continue;

        if (c == '\n') {
            line[lpos] = '\0';
            bool is_nmea  = (lpos > 0 && line[0] == '$');
            bool is_blank = (lpos == 0);
            if (!is_nmea && !is_blank) {
                size_t room = (out_size > opos + 1) ? (out_size - opos - 1) : 0;
                size_t n = strlen(line);
                if (n > room) n = room;
                if (n > 0) {
                    memcpy(out + opos, line, n);
                    opos += n;
                    out[opos++] = '\n';
                    out[opos] = '\0';
                }
                if (strcmp(line, "OK") == 0 || strncmp(line, "ERROR", 5) == 0 ||
                    strncmp(line, "+CME ERROR", 10) == 0 || strncmp(line, "+CMS ERROR", 10) == 0) {
                    terminal_seen = true;
                }
            }
            lpos = 0;
        } else if (c != '\r' && lpos < (int)sizeof(line) - 1) {
            line[lpos++] = (char)c;
        }
    }

    xSemaphoreGive(s_uart_mutex);

    if (opos == 0) {
        snprintf(out, out_size, "(no response — modem timeout after %dms; still connected? see 'gps gnss info')", timeout_ms);
    }
    return terminal_seen;
}

void gps_backend_gnss_register_urc_handler(gnss_urc_handler_t handler) {
    s_urc_handler = handler;
}

// ═══════════════════════════════════════════════════════════════
//  AT+CMGS SMS send — see gps_backend_gnss.h for the full contract.
//  Two-stage handshake (wait for '>' prompt, THEN send body+Ctrl-Z) —
//  different shape than gps_backend_gnss_send_raw_at()'s single-shot
//  "send, wait for terminal line" pattern, so it can't reuse that
//  function; the mutex-take/uart_flush_input/write sequence is copied
//  from it for consistency.
// ═══════════════════════════════════════════════════════════════
bool gps_backend_gnss_send_sms(const char *number, const char *message, int timeout_ms) {
    if (!number || !number[0] || !message) return false;

    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "send_sms: UART1 busy (AT passthrough/GNSS bring-up in progress?) — try again");
        return false;
    }

    uart_flush_input(GNSS_UART_NUM);
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);
    uart_write_bytes(GNSS_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(GNSS_UART_NUM, "\r\n", 2);

    // Wait for the '>' prompt — the modem's signal it's ready for the
    // message body. Fixed 5s cap (not timeout_ms) — this first stage is
    // just the modem parsing the command locally, never involves the
    // cellular network, so it should never take long; timeout_ms is
    // reserved for the SECOND stage below (the actual over-the-air send).
    bool got_prompt = false;
    char probe[64] = {0};
    size_t ppos = 0;
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(5000)) {
        uint8_t c;
        if (uart_read_bytes(GNSS_UART_NUM, &c, 1, pdMS_TO_TICKS(50)) <= 0) continue;
        if (c == '>') { got_prompt = true; break; }
        if (ppos < sizeof(probe) - 1) probe[ppos++] = (char)c;
        probe[ppos] = '\0';
        if (strstr(probe, "ERROR")) break;   // e.g. malformed number, SMS service unavailable
    }

    if (!got_prompt) {
        uint8_t esc = 0x1B;
        uart_write_bytes(GNSS_UART_NUM, &esc, 1);   // cancel — don't leave the modem stuck mid-prompt
        xSemaphoreGive(s_uart_mutex);
        ESP_LOGW(TAG, "send_sms to %s: no '>' prompt from AT+CMGS (raw: %.60s)", number, probe);
        return false;
    }

    uart_write_bytes(GNSS_UART_NUM, message, strlen(message));
    uint8_t ctrl_z = 0x1A;
    uart_write_bytes(GNSS_UART_NUM, &ctrl_z, 1);

    // Second stage: wait for "+CMGS: <mr>\r\nOK\r\n" (sent) or "ERROR"/
    // "+CMS ERROR: <n>" (network-side failure) — this IS the over-the-
    // air leg, hence the caller-supplied timeout_ms here.
    char resp[96] = {0};
    size_t rpos = 0;
    bool ok = false;
    start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        uint8_t c;
        if (uart_read_bytes(GNSS_UART_NUM, &c, 1, pdMS_TO_TICKS(50)) <= 0) continue;
        if (rpos < sizeof(resp) - 1) resp[rpos++] = (char)c;
        resp[rpos] = '\0';
        if (strstr(resp, "OK\r\n")) { ok = true; break; }
        if (strstr(resp, "ERROR")) break;
    }

    xSemaphoreGive(s_uart_mutex);
    ESP_LOGI(TAG, "send_sms to %s: %s", number, ok ? "OK" : "FAILED");
    if (!ok) ESP_LOGW(TAG, "  raw: %.90s", resp);
    return ok;
}
