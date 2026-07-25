#pragma once
// ================================================================
// diag.h — "What's using RAM right now?" / "What's actually stored?"
//
// WHY THIS MODULE EXISTS:
//   Every TaxiMeter backend module talks to the network and writes to
//   flash/NVS. When something doesn't work, the first two questions are
//   always "is this a memory problem?" and "what's actually saved right
//   now?" — this module answers both directly, on demand, without
//   having to reason it out from scattered logs.
//
// SERIAL COMMANDS:
//   mem              → free heap, largest contiguous free block,
//                       lowest-ever free heap (the real danger number),
//                       PSRAM (this board always has it)
//   store            → SPIFFS total/used/free space, every file under
//                       /store (reference data + trip JSON) with size,
//                       PLUS the full session_store dump (NVS — token/
//                       driver/vehicle/network/duty/active-trip state)
//   diag help
// ================================================================
#include <stdbool.h>

bool diag_process_command(const char *line);
