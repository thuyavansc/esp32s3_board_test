#pragma once
// ================================================================
// mini_command.h — v4 demo feature: ONE new serial command, no new
// protocol, no new hardware — exists purely to prove an OTA update from
// v3 to v4 actually changed the device's behavior. Type "game" in the
// Serial Monitor after this version is running and it plays a tiny
// number-guessing round entirely over serial.
// ================================================================
#include <stdbool.h>

// Parses "game" / "guess <n>" from the serial command reader.
// Returns true if recognized.
bool mini_command_process(const char *line);
