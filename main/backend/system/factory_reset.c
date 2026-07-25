#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "config.h"
#include "factory_reset.h"
#include "nvs_state.h"

static const char *TAG = "factory_reset";
static bool s_awaiting_passcode = false;

static void _strip(char *s) {
    // Trim trailing CR/LF/spaces the same way the serial reader's line
    // buffer can leave behind.
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = '\0';
}

bool factory_reset_process_line(const char *line_in) {
    char line[64];
    strlcpy(line, line_in, sizeof(line));
    // Skip leading spaces, then trim trailing ones, without mutating the
    // caller's buffer.
    char *line_p = line;
    while (*line_p == ' ') line_p++;
    _strip(line_p);

    if (s_awaiting_passcode) {
        s_awaiting_passcode = false; // one attempt only — re-arm by typing the trigger again

        // Compares against FACTORY_RESET_PASSCODE (config.h) — currently
        // the literal string "1010". Change it there, not here, if you
        // want a different passcode; this file only ever reads the macro.
        if (strcmp(line_p, FACTORY_RESET_PASSCODE) == 0) {
            const esp_partition_t *factory = esp_partition_find_first(
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
            if (!factory) {
                ESP_LOGE(TAG, "No factory partition on this device — cannot reset");
                return true;
            }

            esp_err_t err = esp_ota_set_boot_partition(factory);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set boot partition to factory: %s", esp_err_to_name(err));
                return true;
            }

            // Safety net (added 2026-07-14, doc 72/73's remote-config work) —
            // if a remote-set serverUrl (config.h's ENABLE_SERVER_URL_OVERRIDE)
            // ever pointed this device somewhere unreachable, factory reset is
            // the recovery path: clear it back to "" so the device falls back
            // to its compiled default and can reach a server again. Harmless
            // no-op when the override was never used.
            nvs_state_clear_server_url();

            ESP_LOGW(TAG, "Passcode correct — boot partition set to 'factory'. Rebooting now...");
            vTaskDelay(pdMS_TO_TICKS(300)); // let the log line above actually flush over serial
            esp_restart();
        } else {
            ESP_LOGW(TAG, "Incorrect passcode — factory reset CANCELLED. Nothing changed.");
        }
        return true;
    }

    if (strcmp(line_p, "factory reset") == 0 || strcmp(line_p, "factoryreset") == 0) {
        s_awaiting_passcode = true;
        ESP_LOGW(TAG, "FACTORY RESET requested.");
        ESP_LOGW(TAG, "This will revert the boot partition to 'factory' and reboot immediately.");
        printf("Type the factory reset passcode now to confirm (anything else cancels):\n> ");
        return true;
    }

    return false;
}
