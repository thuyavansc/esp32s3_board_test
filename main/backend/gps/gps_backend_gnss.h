#pragma once
// ================================================================
// gps_backend_gnss.h — A7670E built-in GNSS backend (AT commands)
//
// One of gps_client.c's parallel GPS backends (ENABLE_GPS_GNSS in
// config.h — the DEFAULT active source). The A7670E has a GNSS receiver
// built into the SAME chip as the cellular modem — there is no separate
// GNSS port; you power it on and read it entirely through AT commands
// over the modem's own UART1 (GPIO 18 TX / 17 RX, confirmed reserved,
// see docs 5/6/136/138). See
// docs/TestFunctionalities/esp32s3_board/
// 139_2026-07-25_gnss_agps_implementation_research_and_plan.md and
// 140_2026-07-25_agps_how_it_actually_works_explained.md for the full
// research this is built from.
//
// BRING-UP SEQUENCE (confirmed against 3 independent sources — the
// Waveshare ESP-IDF GNSS sample, the Waveshare Arduino GNSS sample, and
// this repo's own esp32s3_4g_hotspot/gps_manager.c):
//   AT                          → handshake, retried until OK
//   [A-GPS setup, if ENABLE_AGPS — see below]
//   AT+CGNSSPWR=1                → power on the GNSS engine
//   AT+CGNSSTST=1                → start NMEA sentence output
//   AT+CGNSSPORTSWITCH=<args>    → route NMEA to this UART
//   (then: continuous $GPGGA/$GNGGA + $GPRMC/$GNRMC reading, parsed via
//    the SAME shared gps_nmea.c the NEO-6M backend uses)
//
// A-GPS (ENABLE_AGPS, config.h — OFF by default, no SIM inserted yet):
// downloads satellite assist data over the MODEM'S OWN cellular data
// connection (NOT the ESP32's WiFi — these are two separate networks,
// see doc 140) via SUPL, cutting cold-fix time from 35-90s to 1-5s. When
// enabled, this backend checks SIM presence (AT+CPIN?) and network
// registration (AT+CREG?) BEFORE triggering SUPL/XTRA, and falls back
// cleanly to plain (non-assisted) GNSS if either isn't ready — A-GPS is
// a speed optimization on top of GNSS, never a replacement for it.
//
// SERIAL COMMANDS (via gps_client_process_command() delegation, prefix
// "gps gnss "):
//   gps gnss on|off    → power the GNSS engine up/down at runtime
//                         (AT+CGNSSPWR=1/0), independent of ENABLE_GPS_GNSS
//   gps gnss info       → status: AT-ready, enabled, A-GPS state, latest fix
//   gps gnss agps       → manually re-trigger A-GPS setup (useful right
//                          after inserting a SIM, without rebooting —
//                          no-op if ENABLE_AGPS=0)
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

// Starts UART1 + the bring-up/read task. Returns ESP_OK once the task is
// created (the actual AT handshake + GNSS power-on happens asynchronously
// on that task — a real fix may take 30-90s to arrive after this, cold).
esp_err_t gps_backend_gnss_init(void);

// Runtime enable/disable — when disabled, sends AT+CGNSSPWR=0 (actually
// powers the GNSS engine down on the modem, not just muting the ESP32
// side) and stops publishing fixes; re-enabling re-runs the power-on +
// NMEA-start + port-switch sequence.
void gps_backend_gnss_set_enabled(bool enable);
bool gps_backend_gnss_is_enabled(void);

// Backend-specific serial commands (on/off/info/agps) — called by
// gps_client_process_command() for "gps gnss <args>", with the "gnss "
// prefix already stripped (args is just "on"/"off"/"info"/"agps").
bool gps_backend_gnss_process_command(const char *args);
