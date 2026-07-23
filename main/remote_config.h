#pragma once
// ================================================================
// remote_config.h — taxiNumber/smsNumber/emergencyContactNumber/serverUrl,
// added 2026-07-14. Same HTTP-polling pattern as ota_client.c (this
// project has no MQTT anywhere), persisted via nvs_state.c. See config.h's
// ENABLE_REMOTE_CONFIG / REMOTE_CONFIG_* defines and
// docs/TestFunctionalities/ota-updates/ota-from-dotnet/
// ESP32-Telematics-CAN,-OBD-II,-Remote-Config.md for the background.
// ================================================================
#include <stdbool.h>

// Call once from app_main(), after nvs_state_init()/WiFi — spawns the
// periodic check task. No-op (stub) when ENABLE_REMOTE_CONFIG=0.
void remote_config_init(void);

// Fetches the 4 values now (also called by the periodic task and the
// "config check" serial command) and writes any changed field to NVS.
void remote_config_check_now(void);

// "config status" | "config check" from the serial command reader.
// Returns true if recognized.
bool remote_config_process_command(const char *line);
