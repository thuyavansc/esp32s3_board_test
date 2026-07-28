#pragma once
// ================================================================
// sms_commands.h — three-layer SMS command safety gate + dispatch
// (Phase 2, doc 155/159)
//
// "cannot treat all SMS as commands" (your explicit requirement) —
// THREE independent gates, all must pass before anything executes:
//   Layer 1 — sender whitelist (config.h's SMS_COMMAND_SENDER_WHITELIST/
//             COUNT). Empty = reject every command, from anyone.
//   Layer 2 — command prefix (config.h's SMS_COMMAND_PREFIX, "TAXI#").
//             Anything not starting with this is just a normal received
//             SMS — never even considered a command attempt.
//   Layer 3 — passcode (config.h's SMS_COMMAND_PASSCODE), required ONLY
//             for the destructive REBOOT command. The 3 read-only status
//             commands need whitelist+prefix only.
//
// COMMANDS (after "TAXI#", case-sensitive):
//   STATUS            Trip active? fare/distance/speed so far, replied via SMS
//   LOCATE            Current GPS fix (lat/lon/speed/sats), replied via SMS
//   NET               Uplink/WiFi/cellular/hotspot status, replied via SMS
//   REBOOT <passcode> Trip-safety-interlocked remote reboot — see below
//
// REBOOT INTERLOCK (your explicit requirement: "do not lose data mid-
// trip; show a confirm dialog but force reboot after a timeout so the
// command can't be indefinitely evaded"):
//   1. If a trip is active, a BOUNDED trip-sync flush is attempted
//      (trip_sync_run_full_sequence(), capped at
//      SMS_REBOOT_SYNC_FLUSH_TIMEOUT_S — never lets a stuck server call
//      hold up the reboot indefinitely).
//   2. A forced-reboot esp_timer is armed for SMS_REBOOT_CONFIRM_TIMEOUT_S
//      from now — THIS is the actual guarantee ("can't be indefinitely
//      evaded"), independent of the display/LVGL entirely.
//   3. The display shows a confirm dialog (courtesy — lets the driver
//      react/reboot sooner via "Reboot Now") on whatever screen is
//      currently active. Backend code can never call LVGL directly (doc
//      116) — app_main.c's existing dashboard lv_timer polls
//      sms_commands_reboot_ui_pending() once per tick and does the
//      actual confirm_dialog_show() call itself, on the LVGL thread.
// ================================================================
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SMS_CMD_NOT_A_COMMAND = 0,   // body didn't start with SMS_COMMAND_PREFIX — a normal received SMS
    SMS_CMD_REJECTED,             // prefix matched but whitelist/passcode/unknown-command check failed
    SMS_CMD_ACCEPTED,             // recognized, passed every gate, executed
} sms_command_result_t;

// Called by sms_client.c for every received SMS. `sender`/`body` must
// already be NUL-terminated, parsed out of the modem's AT+CMGR response.
sms_command_result_t sms_commands_try_handle(const char *sender, const char *body);

// ── Reboot-interlock GUI hook — see the REBOOT INTERLOCK note above ──
bool sms_commands_reboot_ui_pending(void);

// Copies the message to display and clears the pending flag — call
// ONCE, from the LVGL thread, immediately before showing the dialog.
void sms_commands_reboot_ui_consume(char *out_message, size_t out_size);

// Called by the confirm dialog's "Reboot Now" button (LVGL thread) —
// cancels the forced-timeout esp_timer (if still pending) and reboots
// immediately either way.
void sms_commands_reboot_now(void);
