#pragma once
// ================================================================
// additional_work.h — catch-all for small, standalone new requirements
// that don't belong in ota_client.c/remote_config.c/trips_api.c/etc,
// added 2026-07-15. Each new requirement gets its own clearly-commented
// section in additional_work.c rather than its own file/module, unless
// it grows large enough to deserve one — this exists specifically so a
// quick "can we try X" request has somewhere to live immediately.
//
// First entry: SMS command interception (added 2026-07-15). Real
// requirement: read a command out of an incoming SMS body and act on it
// (e.g. "reboot"). This hardware doesn't have a GSM/SMS modem wired in
// yet, so this proves the COMMAND-EXECUTION logic works now, via a
// serial command standing in for "an SMS just arrived with this text" —
// see additional_work.c's sms_command_execute() for the part that's
// meant to be reused unchanged once real SMS hardware exists.
// ================================================================
#include <stdbool.h>

// Call once from app_main(), any time after boot — no network dependency.
void additional_work_init(void);

// "sms <command text>" from the serial command reader — simulates an
// incoming SMS with that body. Returns true if recognized (the "sms"
// prefix matched), regardless of whether the command text itself was
// understood.
bool additional_work_process_command(const char *line);
