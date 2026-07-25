#pragma once
// ================================================================
// gps_backend_neo6m.h — u-blox NEO-6M NMEA GPS backend
//
// One of gps_client.c's PARALLEL GPS backends (ENABLE_GPS_NEO6M in
// config.h — runs alongside GNSS, different UART, no conflict). Reads
// NMEA sentences from a NEO-6M module over a dedicated UART and
// publishes each valid fix into the shared dispatcher via
// gps_client_publish_neo6m_fix() — see gps_client.h. Parsing goes
// through the shared gps_nmea.c parser (also used by the GNSS backend).
//
// Ported near-verbatim from esp32_display_taxi_meter's own
// backend/gps_client.c — this is the code already tested on an S3
// board this session; only the "own the shared state" role changed
// (now publishes through the dispatcher instead of owning it directly),
// plus a runtime enable/disable control added.
//
// HARDWARE WIRING (pins from config.h — NEO6M_UART_NUM/TX/RX):
//   NEO-6M VCC → 3.3V (or 5V if the module has its own regulator)
//   NEO-6M GND → GND
//   NEO-6M TX  → ESP32 RX pin (NEO6M_UART_RX)
//   NEO-6M RX  → ESP32 TX pin (NEO6M_UART_TX) [optional — we only read]
//
// SERIAL COMMANDS (via gps_client_process_command() delegation, prefix
// "gps neo6m "):
//   gps neo6m on|off        → enable/disable (module keeps physical
//                              power; disabling just stops parsing/
//                              publishing — no software power-down AT
//                              command exists for a dumb NMEA module)
//   gps neo6m info          → status: running, enabled, fix, read counts
//   gps neo6m start         → continuous logging (every 2s)
//   gps neo6m stop          → stop logging
//   gps neo6m once          → single reading, printed immediately
//   gps neo6m every <sec> [cnt]   → interval + optional count
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

// Starts UART + the NMEA-read task. Returns ESP_OK once the UART is
// configured (a real GPS fix may take a while to arrive after this).
esp_err_t gps_backend_neo6m_init(void);

// Runtime enable/disable — disabled means the read task keeps draining
// the UART (avoids buffer overflow) but stops parsing/publishing fixes.
void gps_backend_neo6m_set_enabled(bool enable);
bool gps_backend_neo6m_is_enabled(void);

// Backend-specific serial commands (on/off/info/start/stop/once/every) —
// called by gps_client_process_command() for "gps neo6m <args>", with
// the "neo6m " prefix already stripped (args is just "start"/"info"/etc).
bool gps_backend_neo6m_process_command(const char *args);
