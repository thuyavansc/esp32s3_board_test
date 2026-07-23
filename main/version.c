#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "config.h"
#include "version.h"

static const char *TAG = "version";

void version_get_string(char *out, size_t out_len) {
#if VERSION_DISPLAY_LEGACY_SIXFIELD
    // LEGACY SCHEME (doc 46) — kept, not deleted. Active only when
    // config.h's VERSION_DISPLAY_LEGACY_SIXFIELD is flipped to 1.
    snprintf(out, out_len, "%02d.%02d.%02d.%02d.%02d.%02d",
             FW_VERSION_MAJOR, FW_VERSION_MINOR,
             FW_VERSION_YEAR, FW_VERSION_MONTH, FW_VERSION_DAY,
             FW_VERSION_BUILD);
#else
    // ACTIVE SCHEME (2026-07-13+, doc 53) — matches the company's real
    // ASP.NET Core backend's own display convention exactly: a plain
    // "YY.MM.DD" date string (e.g. "26.07.13"). FW_VERSION_CODE (a
    // separate integer, see version_get_code()) is what identifies the
    // exact release and drives the OTA "is this newer" decision — this
    // string is purely a date label now, same division of roles as their
    // backend's own Version (string) + VersionCode (int) pair.
    snprintf(out, out_len, "%02d.%02d.%02d",
             FW_VERSION_YEAR, FW_VERSION_MONTH, FW_VERSION_DAY);
#endif
}

// Legacy fallback comparison only now — ota_client.c uses this SOLELY when
// a server response doesn't include a "versionCode" field at all (e.g. an
// old/legacy test-server reply). FW_VERSION_MAJOR/MINOR stay defined
// regardless of VERSION_DISPLAY_LEGACY_SIXFIELD so this keeps working.
int version_get_major_minor(void) {
    return FW_VERSION_MAJOR * 100 + FW_VERSION_MINOR;
}

int version_get_code(void) {
    return FW_VERSION_CODE;
}

void version_print_banner(void) {
    char ver[24];
    version_get_string(ver, sizeof(ver));

    // Which partition this exact binary booted from — meaningful now that
    // this board's partition table has factory/ota_0/ota_1 (partitions.csv).
    // See docs/TestFunctionalities/ota-updates/46_..._ota_update_complete_guide.md
    // Part 3 for what "currently running from ota_1" etc. actually means.
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "FIRMWARE VERSION");
    ESP_LOGI(TAG, "--------------------------------------");
    ESP_LOGI(TAG, "  Project:   %s", FW_PROJECT_NAME);
    ESP_LOGI(TAG, "  Version:   %s (code %d)", ver, FW_VERSION_CODE);
    ESP_LOGI(TAG, "  Build:     %s", FW_BUILD_LABEL);
    ESP_LOGI(TAG, "  Running from partition: %s",
             running ? running->label : "(unknown)");
}
