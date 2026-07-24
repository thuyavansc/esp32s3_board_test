#pragma once
// ================================================================
// llm_runner.h — serial-console adapter for llm.c (TinyLlama-260K,
// ported from https://github.com/DaveBben/esp32-llm), new for
// esp32s3_board.
//
// llm.c/llm.h are the port's own inference engine, essentially
// unmodified (see doc 125). This file is entirely new — it replaces
// that reference project's own app_main()-embedded OLED/u8g2 chat loop
// with a thin wrapper matching this project's existing serial-command
// convention (ram_test.c's ram_test_process_command(), etc.).
//
// Deliberately serial-console-only: no coupling with this project's
// display/touch/UI code at all (doc 123 §7a) — every output byte goes
// through printf()/ESP_LOGI() over the same UART serial_cmd_task
// already reads commands from.
// ================================================================
#include <stdbool.h>

// Call once from app_main() (mounts the dedicated `llm` SPIFFS
// partition, config.h's LLM_MOUNT_POINT — does NOT load the model yet;
// that's deferred to the first "llm run" command, since it's a ~1MB+
// read/malloc + two Core-1-pinned task creations, not something to pay
// for at boot if the feature is never actually used this session).
void llm_runner_init(void);

// "llm run <prompt...>" | "llm run <steps> <prompt...>" | "llm help"
// from the serial command reader. First call lazily loads the model
// (logs progress); every call after that reuses the already-loaded
// model. Generation itself runs synchronously (blocks the calling
// task — serial_cmd_task — for the duration, same as this project's
// existing "ram test psram"/"api get" commands already do). Returns
// true if recognized.
bool llm_runner_process_command(const char *line);
