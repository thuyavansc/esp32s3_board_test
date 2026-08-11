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
//   stacks           → per-task stack high-water marks (doc 184 §10.2 —
//                       measure margin instead of guessing it; this is
//                       the instrumentation that was missing when
//                       trip_tick's 4096-byte stack silently overflowed
//                       3 times before anyone could see it coming)
//   diag help
// ================================================================
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool diag_process_command(const char *line);

// doc 184 §10.2 — registers a task so 'stacks' can report its high-
// water mark. Call once, right after xTaskCreate(), passing the SAME
// handle xTaskCreate() wrote into its own last (out) parameter — that
// parameter is commonly left NULL/discarded across this codebase; each
// call site this is wired into now captures it instead.
void diag_register_task(TaskHandle_t handle, const char *name);
