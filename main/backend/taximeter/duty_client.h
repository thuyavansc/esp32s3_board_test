#pragma once
// ================================================================
// duty_client.h — On-duty / off-duty
//
// DELIBERATELY simple, matching the Android reference exactly: local
// duty-status flips IMMEDIATELY (session_store, no network wait), then
// the server call fires in the background and its result is discarded
// — no retry, no queue. This is not a shortcut; it's the reference
// app's actual design.
//
// SERIAL COMMANDS:
//   duty on     → GET taxis-api/api/DriverStatus/OnDuty (fire-and-forget)
//   duty off    → GET taxis-api/api/DriverStatus/OffDuty (fire-and-forget)
//   duty info   → show current duty status
// ================================================================
#include <stdbool.h>

void duty_client_go_on_duty(void);
void duty_client_go_off_duty(void);

bool duty_client_process_command(const char *line);
