#pragma once
// ================================================================
// sms_client.h — SMS receive/log/send runtime (Phase 2, doc 155/159)
//
// Owns:
//   - Modem SMS setup: AT+CMGF=1 (text mode) + AT+CNMI=2,1,0,0,0 (URC on
//     new message), sent once at init via gps_client_send_raw_at() (the
//     EXISTING, proven AT passthrough — no new UART code needed here).
//   - A dedicated task that turns incoming "+CMTI:" URCs (registered
//     via gps_client_register_sms_urc_handler()) into AT+CMGR reads,
//     hands each message to sms_commands_try_handle() for the safety-
//     gated command path, appends it to an in-RAM inbox, and deletes it
//     off the SIM (AT+CMGD) once processed. Also runs a periodic
//     "AT+CMGL=\"REC UNREAD\"" reliability sweep to catch any message
//     whose URC was missed (e.g. arrived during boot, before the
//     handler was registered).
//   - Outbound send: a thin call-through to gps_client_send_sms().
//
// WHY NO NVS/SPIFFS PERSISTENCE FOR THE INBOX: keeping this task's own
// call graph flash-free keeps the door open for a future PSRAM-stack
// conversion (config.h's ENABLE_PSRAM_TASK_STACKS comment — not done in
// this pass, kept conservative). "log" (your requirement) is satisfied
// via ESP_LOGI at receive time PLUS this in-RAM ring buffer (GUI inbox
// + "smsc list") — it just doesn't survive a reboot, the same trade-off
// this project already accepts for e.g. the dashboard's own live totals.
//
// SERIAL COMMANDS ("smsc ..." — NOT "sms", see config.h's own comment
// for why: "sms <command text>" is a pre-existing, unrelated bench-test
// simulator in additional_work.c, left untouched):
//   smsc send <number> <message...>   Send a real SMS (routed via bg_worker)
//   smsc list                          Last SMS_INBOX_CAPACITY received (sender + body)
//   smsc storage                       EVERY SMS on the SIM (not just our own inbox above)
//                                       + used/total capacity (doc 162 §2 — old pre-existing
//                                       messages our reliability sweep never touches)
//   smsc delete <index>                Delete one SMS off the SIM (AT+CMGD)
//   smsc delete all                    Delete every SMS on the SIM (AT+CMGD=1,4)
//   smsc status                        Ready state, counts, whitelist size
//   smsc help
// ================================================================
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef struct {
    char    sender[24];
    char    body[161];
    time_t  received_at;   // wall-clock, best-effort (0 if SNTP hasn't synced yet)
    bool    was_command;   // true if the body matched SMS_COMMAND_PREFIX (whether accepted or rejected)
} sms_inbox_entry_t;

typedef struct {
    bool     ready;              // AT+CMGF/CNMI setup completed successfully
    uint32_t received_total;
    uint32_t commands_accepted;
    uint32_t commands_rejected;  // whitelist/passcode/unknown-command failures
    time_t   last_received_at;
} sms_client_stats_t;

// Starts the SMS runtime. Call once, AFTER gps_client_init() (needs the
// GNSS backend's UART1 + mutex already up — see app_main.c's boot
// ordering) and ideally after trip_manager_init() (sms_commands.c's
// STATUS command reads live trip state). Gated by ENABLE_SMS at the
// call site in app_main.c, same pattern as every other optional module.
esp_err_t sms_client_init(void);

// Newest-first copy of up to max_count inbox entries. Safe from any task.
int sms_client_get_inbox(sms_inbox_entry_t *out, int max_count);
int sms_client_get_inbox_count(void);
void sms_client_get_stats(sms_client_stats_t *out);

// Serial command handler ("smsc ...").
bool sms_client_process_command(const char *line);
