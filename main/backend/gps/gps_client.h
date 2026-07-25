#pragma once
// ================================================================
// gps_client.h — GPS dispatcher with THREE selectable backends
//
// WHAT THIS MODULE DOES:
//   fare_calc.c / trip_manager.c only ever call gps_client_get_latest()
//   — they don't know or care where the fix came from. This dispatcher
//   owns the one shared gps_data_t and routes gps_client_init() /
//   gps_client_process_command() to whichever backend is selected at
//   compile time via GPS_SOURCE (config.h).
//
// THE THREE BACKENDS (see docs/TestFunctionalities/esp32s3_board/
// 135_2026-07-25_fare_calc_and_backend_integration_plan.md §4 and
// 136_2026-07-25_gpio_uart_conflict_analysis_and_board_identity_question.md):
//
//   GPS_SRC_NEO6M  — u-blox NEO-6M NMEA parser over a dedicated UART.
//                    Ported near-verbatim from esp32_display_taxi_meter's
//                    gps_client.c — ALREADY TESTED on an S3 board this
//                    session. Full serial command set: gps start/stop/
//                    once/every/info.
//
//   GPS_SRC_INJECT — No GPS hardware at all. The PC GUI (or a human)
//                    feeds fixes over the same USB serial the rest of
//                    the console uses: "gps set <lat> <lon> <speed_kmh>
//                    [hdop] [sats]". Lets a full trip (setup → auth →
//                    duty → trip start → drive → trip stop) be exercised
//                    with zero GPS hardware attached — this is how the
//                    fare-calc integration gets tested from the PC GUI.
//
//   GPS_SRC_GNSS   — NOT YET IMPLEMENTED. Intended as the real
//                    deployment default (A7670E built-in GNSS + A-GPS/
//                    SUPL, per doc 17) once doc 136's board-identity
//                    question is answered — the A7670E's AT-command
//                    UART is GPIO 18(TX)/17(RX), confirmed working and
//                    reserved (do not reuse those pins for anything
//                    else). Selecting GPS_SRC_GNSS today logs a clear
//                    "not implemented yet" message and gps_client_init()
//                    falls back to returning no fix — it does NOT
//                    silently pretend to talk to hardware that may not
//                    be attached to this exact board.
//
//   "gps set ..." (the injection command) is ALWAYS compiled in and
//   always accepted, regardless of GPS_SOURCE — this lets the PC GUI
//   override/feed a fix for testing even while a NEO-6M backend is
//   selected, without needing a rebuild.
//
// SERIAL COMMANDS (dispatched from app_main.c's serial_cmd_task):
//   gps set <lat> <lon> <speed_kmh> [hdop] [sats]   → inject a fix (always available)
//   gps info                                         → show current fix + which backend is active
//   gps start | stop | once | every <sec> [cnt]      → NEO-6M backend only (no-ops on other backends)
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

// Init the selected GPS backend (GPS_SOURCE in config.h). Call once,
// after WiFi connects (matches every other backend module's ordering).
esp_err_t gps_client_init(void);

// Get the latest GPS data (returns pointer to static struct, NULL if no
// fix yet) — fare_calc.c calls this every tick.
const gps_data_t *gps_client_get_latest(void);

// Get GPS status string for logging/serial monitor.
const char *gps_client_get_status(void);

// Process a "gps ..." serial command. Handles "gps set"/"gps info"
// itself (always available); delegates anything else to the active
// backend's own process_command(), if it defines one.
bool gps_client_process_command(const char *line);

// True if the active backend is running/initialized.
bool gps_client_is_running(void);

// ── Internal — for backend implementations ONLY (gps_backend_*.c) ──
// Publishes a new fix into the shared state every backend reads through
// gps_client_get_latest(). Not part of the public consumer API.
void gps_client_set_fix(const gps_data_t *fix);
