#pragma once
// ================================================================
// gps_client.h — GPS dispatcher: TWO backends running in PARALLEL,
// plus always-on serial injection
//
// WHAT THIS MODULE DOES:
//   fare_calc.c / trip_manager.c only ever call gps_client_get_latest()
//   — they don't know or care where the fix came from. This dispatcher
//   owns ONE fix slot per backend and hands the currently-selected
//   "active source"'s fix to callers, while ALL enabled backends keep
//   running and publishing independently in the background — so you can
//   run GNSS as the real fare source while NEO-6M logs/compares in
//   parallel for a different purpose, or vice versa. See
//   docs/TestFunctionalities/esp32s3_board/
//   139_2026-07-25_gnss_agps_implementation_research_and_plan.md §5.
//
// THE THREE SOURCES:
//
//   GPS_SRC_GNSS (default active source) — A7670E's built-in GNSS,
//     driven entirely over the modem's own AT-command UART1 (GPIO 18
//     TX / 17 RX) — see gps_backend_gnss.c. Includes A-GPS/SUPL, staged
//     behind ENABLE_AGPS (config.h) until a data-enabled SIM is
//     inserted (doc 140 explains exactly why a SIM is required and what
//     still runs on the ESP32 side vs. entirely inside the modem).
//
//   GPS_SRC_NEO6M — external u-blox NEO-6M module, NMEA over a
//     dedicated UART2 — see gps_backend_neo6m.c. Already tested on real
//     S3 hardware. Kept compiled and runnable ALONGSIDE GNSS (different
//     UART, no conflict) for bench-testing or a second, independent
//     purpose.
//
//   GPS_SRC_INJECT — no GPS hardware at all. The PC GUI (or a human)
//     feeds fixes over the same USB serial the rest of the console
//     uses: "gps set <lat> <lon> <speed_kmh> [hdop] [sats]". ALWAYS
//     compiled in and always accepted, regardless of which backends are
//     enabled — this is how the fare-calc integration gets tested with
//     zero GPS hardware attached. Also the automatic fallback: if the
//     active source has no fix yet, an injected fix is used instead.
//
// Each backend can be compiled in/out at build time (ENABLE_GPS_GNSS /
// ENABLE_GPS_NEO6M, config.h) AND enabled/disabled at RUNTIME
// independently ("gps gnss on|off", "gps neo6m on|off") without a
// rebuild — your explicit requirement: "make sure we have control of
// gnss enable and disable also neo-6m".
//
// SERIAL COMMANDS (dispatched from app_main.c's serial_cmd_task):
//   gps set <lat> <lon> <speed_kmh> [hdop] [sats]   → inject a fix (always available)
//   gps info                                         → dispatcher status: active source + every backend's fix
//   gps source gnss|neo6m|inject                     → choose which backend feeds fare_calc
//   gps gnss  on|off|info|agps                        → GNSS backend (see gps_backend_gnss.h)
//   gps neo6m on|off|info|start|stop|once|every ...   → NEO-6M backend (see gps_backend_neo6m.h)
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

// GPS data structure — shared by every backend and every consumer
// (fare_calc.c, trip_manager.c, rest_api_storage.c's future trip-sync).
typedef struct {
    double lat;          // Latitude (decimal degrees, >0 = N, <0 = S)
    double lon;          // Longitude (decimal degrees, >0 = E, <0 = W)
    double alt;          // Altitude (meters)
    double speed;        // Speed (km/h)
    double course;       // Course/track (degrees, 0-360)
    double hdop;         // Horizontal dilution of precision
    int    satellites;   // Number of satellites used
    int    fix_quality;  // 0=no fix, 1=GPS fix, 2=DGPS fix
    bool   has_fix;      // true if valid position available
} gps_data_t;

// Which backend's fix gps_client_get_latest() returns to callers.
// Runtime-switchable via "gps source ..." — no rebuild needed.
typedef enum {
    GPS_SRC_GNSS   = 0,
    GPS_SRC_NEO6M  = 1,
    GPS_SRC_INJECT = 2,
} gps_source_t;

// Starts every ENABLE_GPS_* backend compiled in (config.h). Call once,
// after WiFi connects (matches every other backend module's ordering).
esp_err_t gps_client_init(void);

// Get the ACTIVE source's latest fix (falls back to an injected fix if
// the active source has none yet). Returns NULL if nothing has a fix at
// all. fare_calc.c calls this every tick.
const gps_data_t *gps_client_get_latest(void);

// Get GPS status string for logging/serial monitor (reflects the active source).
const char *gps_client_get_status(void);

// Process a "gps ..." serial command — handles "set"/"info"/"source"
// itself, delegates "gnss ..."/"neo6m ..." to the matching backend.
bool gps_client_process_command(const char *line);

// Raw AT-command passthrough to the modem chip (A7670E) — forwards to
// gps_backend_gnss.c, the only owner of that UART. `cmd` is sent exactly
// as given (no wrapper — a real AT command like "AT+CSQ"). `out` gets
// filled with the modem's response (NMEA lines filtered out) or an
// explanatory message if the GNSS backend isn't compiled into this
// build. Independent of "gps source ..." — always talks to the modem
// itself, regardless of which GPS source is currently active.
bool gps_client_send_raw_at(const char *cmd, char *out, size_t out_size, int timeout_ms);

// True once gps_client_init() has run.
bool gps_client_is_running(void);

// Which backend currently feeds fare_calc, and how to change it.
void gps_client_set_active_source(gps_source_t src);
gps_source_t gps_client_get_active_source(void);

// ── Internal — for backend implementations ONLY (gps_backend_*.c) ──
// Each backend publishes into its OWN fix slot — they never overwrite
// each other, which is what makes true parallel operation possible.
void gps_client_publish_gnss_fix(const gps_data_t *fix);
void gps_client_publish_neo6m_fix(const gps_data_t *fix);
