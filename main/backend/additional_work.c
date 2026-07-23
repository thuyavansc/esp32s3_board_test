#include "config.h"

#if ENABLE_ADDITIONAL_WORK

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "additional_work.h"

static const char *TAG = "additional_work";

// ════════════════════════════════════════════════════════════════
//  REQUIREMENT: SMS command interception (added 2026-07-15)
//
//  Real goal: an incoming SMS's body is read for a known command word
//  and acted on (e.g. "reboot"). No GSM/SMS modem is wired into this
//  hardware yet, so this section proves the ONE part that actually
//  matters right now — the command-parsing-and-execution logic — using
//  a serial command as the stand-in trigger.
//
//  When real SMS hardware is added later: wire its "message received"
//  callback to call sms_command_execute(sms_body_text) directly instead
//  of going through additional_work_process_command()'s serial-line
//  parsing above it — nothing about THIS function needs to change.
// ════════════════════════════════════════════════════════════════
static bool sms_command_execute(const char *command_text) {
    while (*command_text == ' ') command_text++;

    if (strcasecmp(command_text, "reboot") == 0 || strcasecmp(command_text, "restart") == 0) {
        ESP_LOGW(TAG, "SMS command 'reboot' — rebooting now...");
        vTaskDelay(pdMS_TO_TICKS(300)); // let the log line above flush over serial first
        esp_restart(); // does not return
        return true; // unreachable
    }

    // Add more recognized commands here as they're actually needed —
    // same pattern: match the word, do the thing, return true.

    ESP_LOGW(TAG, "SMS command not recognized: '%s'", command_text);
    return false;
}

void additional_work_init(void) {
    ESP_LOGI(TAG, "Additional work module ready (try: \"sms reboot\")");
}

bool additional_work_process_command(const char *line) {
    while (*line == ' ') line++;
    if (strncmp(line, "sms", 3) != 0) return false;

    const char *p = line + 3;
    while (*p == ' ') p++;

    if (*p) {
        ESP_LOGI(TAG, "Simulating incoming SMS, body: \"%s\"", p);
        if (!sms_command_execute(p)) {
            printf("  Unrecognized SMS command: '%s'\n", p);
        }
    } else {
        printf("  usage: sms <command text>   e.g. \"sms reboot\"\n");
        printf("  Recognized commands: reboot / restart\n");
    }
    return true;
}

#else // !ENABLE_ADDITIONAL_WORK

#include "additional_work.h"

void additional_work_init(void) {}
bool additional_work_process_command(const char *line) { (void)line; return false; }

#endif // ENABLE_ADDITIONAL_WORK
