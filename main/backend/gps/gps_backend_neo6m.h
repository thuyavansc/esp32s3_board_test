#pragma once
// ================================================================
// gps_backend_neo6m.h — u-blox NEO-6M NMEA GPS backend
//
// One of gps_client.c's three selectable backends (GPS_SOURCE ==
// GPS_SRC_NEO6M in config.h). Reads NMEA sentences ($GPGGA/$GNGGA,
// $GPRMC/$GNRMC) from a NEO-6M module over a dedicated UART and
// publishes each valid fix into the shared dispatcher via
// gps_client_set_fix() — see gps_client.h.
//
// Ported near-verbatim from esp32_display_taxi_meter's own
// backend/gps_client.c (UART2, GPIO16/17 on classic WROOM) — this is
// the code already tested on an S3 board this session, so its NMEA
// parsing logic is unchanged; only the "own the shared state" role is
// removed in favor of publishing through gps_client_set_fix().
//
// HARDWARE WIRING (pins from config.h — GPS_UART_NUM/TX/RX):
//   NEO-6M VCC → 3.3V (or 5V if the module has its own regulator)
//   NEO-6M GND → GND
//   NEO-6M TX  → ESP32 RX pin (GPS_UART_RX)
//   NEO-6M RX  → ESP32 TX pin (GPS_UART_TX) [optional — we only read]
//
// SERIAL COMMANDS (via gps_client_process_command() delegation):
//   gps start              → Start continuous GPS reading/logging
//   gps stop                → Stop GPS reading
//   gps once                → Get one GPS reading and print
//   gps every <sec> [cnt]   → Read every X seconds, Y times
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

// Starts UART + the NMEA-read task. Returns ESP_OK once the UART is
// configured (a real GPS fix may take a while to arrive after this).
esp_err_t gps_backend_neo6m_init(void);

// Backend-specific serial commands (start/stop/once/every/info) —
// called by gps_client_process_command() when GPS_SOURCE == GPS_SRC_NEO6M.
bool gps_backend_neo6m_process_command(const char *line);
