/**
 * app_main.c — esp32s3_board: ESP32-S3 N16R8V board bring-up diagnostic
 * firmware + full TaxiMeter backend (fare calculation, provisioning,
 * login, duty, reference data, trip lifecycle), ported from
 * esp32_chip_info_test/main/app_main.c and
 * esp32_display_taxi_meter/main/app_main.c.
 *
 * See docs/TestFunctionalities/esp32s3_board/
 * 135_2026-07-25_fare_calc_and_backend_integration_plan.md for the full
 * integration plan and rationale.
 *
 * Base behaviour unchanged from the original diagnostic tool: prints
 * chip/flash/RAM/partition/NVS/SPIFFS info once at boot, "getinfo"/
 * "info" reprints it on demand. Changes vs. the original for this
 * project:
 *
 *   - RAM/heap report extended to show internal SRAM and PSRAM
 *     side-by-side (all live-queried — esp_flash_get_size()/
 *     esp_chip_info()/heap_caps_get_*() — never hardcoded), plus PSRAM
 *     detection (esp_psram_is_initialized()/esp_psram_get_size()).
 *   - ram_test.c added — "ram info"/"ram test sram/psram/all" serial
 *     commands, plus a periodic PSRAM health-check task (see
 *     ram_test_init()).
 *   - WiFi now connects whenever ENABLE_WIFI (config.h) is on,
 *     independent of ENABLE_OTA/ENABLE_TRIPS_API/ENABLE_REMOTE_CONFIG —
 *     previously WiFi only started if one of those three needed it.
 *   - serial_cmd_task stack raised 4096 -> 8192.
 *   - OTA (ota_client.c) and Remote-Config (remote_config.c) are fully
 *     present, unchanged, just switched off via config.h (ENABLE_OTA=0,
 *     ENABLE_REMOTE_CONFIG=0).
 *   - Display feature added (ported from esp32_display_taxi_3, ESP32-S3
 *     pins only — see config.h): bg_worker_init() early (before WiFi
 *     touches the heap), display_init()/touch_init()/ui_init() after
 *     WiFi connects, lv_tick_task started (tick-only, safe from any
 *     task) + an lv_timer feeding the Dashboard from the LIVE meter
 *     (fare_calc_get_snapshot() — zeros when idle, real totals when a
 *     trip is running; NOT the old simulated sweep), and app_main()'s
 *     own final loop is now the LVGL handler loop — app_main()
 *     intentionally no longer returns.
 *   - TaxiMeter backend added (backend/taximeter/, backend/gps/): SNTP
 *     time sync (tariffs need real wall-clock time), session_store,
 *     setup_client (network+vehicle provisioning), auth_client (login),
 *     duty_client, reference_data (tariffs/fixed-rates/special-fares/
 *     holidays), fare_calc + trip_manager (the actual meter), and a
 *     3-backend GPS dispatcher (gps_client.c — NEO-6M / serial
 *     injection from the PC GUI / GNSS-A7670E [not yet implemented]).
 *   - backend/ reorganized into subfolders (system/, taximeter/, gps/)
 *     — was previously a flat dump of ~10 files.
 *   - trips_api.c/.h REMOVED (redundant — same URL, same job as
 *     rest_api_storage.c, which is now the single SPIFFS-mount owner +
 *     "api" command handler; also backs the Display trip screen's
 *     list/read/delete helpers).
 *   - Trip-to-server sync added (docs/TestFunctionalities/esp32s3_board/
 *     calculations-impl/151_..._esp32_vs_android_gap_analysis_and_
 *     implementation_plan.md, D1-D5): fare_calc.c now keeps a full
 *     per-segment TimeFrame history (not a single running total),
 *     directions_client.c road-snaps any GPS gap over
 *     DIRECTIONS_MIN_DISTANCE_M via GraphHopper, and trip_sync.c runs
 *     the AddJob->Trips->SaveJobFares sequence (driver identity, Bearer
 *     token, with a re-login-once-on-401 retry) — wired into
 *     trip_manager.c's start/tick/finalize lifecycle.
 *
 * ================================================================
 * SERIAL COMMANDS (available ones depend on which flags are ON)
 * ================================================================
 *   getinfo / info     Full chip/flash/RAM(SRAM+PSRAM)/partition/storage report
 *   getversion         Firmware version + build label + running partition
 *   ram info           Live SRAM+PSRAM totals/free/largest-block (no allocation)
 *   ram test sram|psram|all   Allocate/write/verify/free real-value RAM test
 *   ota check/status   [ENABLE_OTA, currently OFF]
 *   api get/read/list/delete/info/help  Trip fetch/storage (rest_api_storage.c)
 *   gps set|info|start|stop|once|every  GPS — inject a fix or drive a NEO-6M module
 *   setup network|vehicle|info|help     Network passcode + vehicle provisioning
 *   auth login|logout|info|help         Driver login/logout
 *   ref fetch|list|info|help            Tariffs/fixed-rates/special-fares/holidays
 *   duty on|off|info                    On-duty / off-duty
 *   trip start|stop|pause|resume|extras|finalize|info   The meter itself
 *   sync now|status                     Force/inspect AddJob->Trips->SaveJobFares sync (trip_sync.c)
 *   directions test <lat1> <lon1> <lat2> <lon2>   Manually exercise the GraphHopper road-distance call
 *   session info|clear                  NVS session dump / logout-style reset
 *   mem / store                         Heap + SPIFFS/NVS diagnostics
 *   game / guess <n>   [ENABLE_MINI_COMMAND, currently OFF]
 *   config status|check [ENABLE_REMOTE_CONFIG, currently OFF]
 *   sms <command>      [ENABLE_ADDITIONAL_WORK]
 *   llm run <prompt>   [ENABLE_LLM]  TinyLlama-260K local inference, serial-only (doc 123/125)
 *   factory reset      always on   Two-step, passcode-gated revert to the
 *                                  `factory` partition + reboot.
 *   nvs status|company|product  always on
 * ================================================================
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_idf_version.h"
#include "esp_private/esp_clk.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "lvgl.h"
#include "config.h"
#include "version.h"
#include "backend/system/ota_client.h"
#include "backend/system/mini_command.h"
#include "backend/system/factory_reset.h"
#include "backend/system/nvs_state.h"
#include "backend/system/remote_config.h"
#include "backend/system/additional_work.h"
#include "backend/system/ram_test.h"
#include "backend/system/net_diag.h"
#include "backend/bg_worker.h"
#include "backend/taximeter/session_store.h"
#include "backend/taximeter/setup_client.h"
#include "backend/taximeter/auth_client.h"
#include "backend/taximeter/duty_client.h"
#include "backend/taximeter/reference_data.h"
#include "backend/taximeter/fare_calc.h"
#include "backend/taximeter/directions_client.h"
#include "backend/taximeter/trip_sync.h"
#include "backend/taximeter/trip_manager.h"
#include "backend/taximeter/diag.h"
#include "backend/taximeter/rest_api_storage.h"
#include "backend/gps/gps_client.h"
#include "backend/network/wifi/wifi_sta.h"
#include "backend/network/cellular/cellular_ppp.h"
#include "backend/network/hotspot/hotspot_nvs.h"
#include "backend/network/hotspot/hotspot_ap.h"
#include "backend/network/net_manager.h"
#include "backend/network/sms/sms_client.h"
#include "backend/network/sms/sms_commands.h"
#include "display/display_driver.h"
#include "display/touch_driver.h"
#include "display/ui_main.h"
#include "display/network/network_screen.h"
#include "display/sms/sms_screen.h"
#include "display/login/login_screen.h"
#include "ui_components/confirm_dialog.h"
#include "llm/llm_runner.h"

static const char *TAG = "chipinfo";

static bool s_spiffs_mounted = false;

// ═══════════════════════════════════════════════════════════════
//  Helper — format a byte count as "N bytes | K.K KB (M.MM MB)"
// ═══════════════════════════════════════════════════════════════
static void format_size(uint64_t bytes, char *out, size_t out_len) {
    double kb = bytes / 1024.0;
    double mb = bytes / (1024.0 * 1024.0);
    snprintf(out, out_len, "%llu bytes | %.1f KB (%.2f MB)",
             (unsigned long long)bytes, kb, mb);
}

// ═══════════════════════════════════════════════════════════════
//  IDENTITY BANNER — unchanged from the original tool
// ═══════════════════════════════════════════════════════════════
static void print_identity_banner(void) {
    esp_chip_info_t info;
    esp_chip_info(&info);

    const char *model_str = "Unknown";
    switch (info.model) {
        case CHIP_ESP32:   model_str = "ESP32";    break;
        case CHIP_ESP32S2: model_str = "ESP32-S2";  break;
        case CHIP_ESP32S3: model_str = "ESP32-S3";  break;
        case CHIP_ESP32C3: model_str = "ESP32-C3";  break;
        default: break;
    }

    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    int cpu_mhz  = esp_clk_cpu_freq() / 1000000;
    int xtal_mhz = esp_clk_xtal_freq() / 1000000;

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "IDENTITY BANNER  (live-queried from the running chip)");
    ESP_LOGI(TAG, "--------------------------------------");
    ESP_LOGI(TAG, "  Chip is %s (revision v%d.%d)",
             model_str, info.revision / 100, info.revision % 100);
    ESP_LOGI(TAG, "  Features: %s%s%s%s%dMHz",
             (info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi, " : "",
             (info.features & CHIP_FEATURE_BT)       ? "BT, " : "",
             (info.features & CHIP_FEATURE_BLE)      ? "BLE, " : "",
             (info.cores > 1) ? "Dual Core, " : "Single Core, ",
             cpu_mhz);
    ESP_LOGI(TAG, "  Crystal is %d MHz", xtal_mhz);
    ESP_LOGI(TAG, "  MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void print_chip_info(void) {
    esp_chip_info_t info;
    esp_chip_info(&info);

    const char *model_str = "Unknown";
    switch (info.model) {
        case CHIP_ESP32:   model_str = "ESP32";    break;
        case CHIP_ESP32S2: model_str = "ESP32-S2";  break;
        case CHIP_ESP32S3: model_str = "ESP32-S3";  break;
        case CHIP_ESP32C3: model_str = "ESP32-C3";  break;
        default: break;
    }

    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "CHIP INFO  (live — from esp_chip_info() / efuses)");
    ESP_LOGI(TAG, "--------------------------------------");
    ESP_LOGI(TAG, "  Model:       %s", model_str);
    ESP_LOGI(TAG, "  Cores:       %d", info.cores);
    ESP_LOGI(TAG, "  Revision:    v%d.%d", info.revision / 100, info.revision % 100);
    ESP_LOGI(TAG, "  Features:    %s%s%s%s",
             (info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
             (info.features & CHIP_FEATURE_BT)       ? "BT " : "",
             (info.features & CHIP_FEATURE_BLE)      ? "BLE " : "",
             (info.features & CHIP_FEATURE_EMB_FLASH) ? "Embedded-Flash" : "External-Flash");
    ESP_LOGI(TAG, "  Base MAC:    %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "  IDF version: %s", esp_get_idf_version());
}

static void print_flash_info(void) {
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    uint32_t flash_id = 0;
    esp_flash_read_id(NULL, &flash_id);

    char size_buf[64];
    format_size(flash_size, size_buf, sizeof(size_buf));

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "FLASH INFO  (live-probed)");
    ESP_LOGI(TAG, "--------------------------------------");
    ESP_LOGI(TAG, "  Detected size:   %s", size_buf);
    ESP_LOGI(TAG, "  JEDEC ID:        0x%06lX", (unsigned long)flash_id);
}

// ═══════════════════════════════════════════════════════════════
//  RAM / HEAP INFO — extended for esp32s3_board to show internal SRAM
//  and PSRAM side-by-side. Every number here is a live heap_caps_*()/
//  esp_psram_*() query against the running chip, never a hardcoded
//  "16MB flash / 8MB PSRAM" assumption from the board name.
// ═══════════════════════════════════════════════════════════════
static void print_heap_info(void) {
    char buf[64];
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "RAM / HEAP INFO  (live)");
    ESP_LOGI(TAG, "--------------------------------------");
    format_size(esp_get_free_heap_size(), buf, sizeof(buf));
    ESP_LOGI(TAG, "  Free heap now (all capabilities): %s", buf);
    format_size(esp_get_minimum_free_heap_size(), buf, sizeof(buf));
    ESP_LOGI(TAG, "  Minimum ever free:                %s", buf);

    bool psram_ok = esp_psram_is_initialized();
    ESP_LOGI(TAG, "  --------------------------------------");
    ESP_LOGI(TAG, "  PSRAM initialized: %s", psram_ok ? "YES" : "NO");
    if (psram_ok) {
        format_size(esp_psram_get_size(), buf, sizeof(buf));
        ESP_LOGI(TAG, "  PSRAM total (esp_psram_get_size): %s", buf);
    }

    format_size(heap_caps_get_total_size(MALLOC_CAP_INTERNAL), buf, sizeof(buf));
    ESP_LOGI(TAG, "  SRAM  total:          %s", buf);
    format_size(heap_caps_get_free_size(MALLOC_CAP_INTERNAL), buf, sizeof(buf));
    ESP_LOGI(TAG, "  SRAM  free now:       %s", buf);
    format_size(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), buf, sizeof(buf));
    ESP_LOGI(TAG, "  SRAM  min free ever:  %s", buf);
    format_size(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), buf, sizeof(buf));
    ESP_LOGI(TAG, "  SRAM  largest block:  %s", buf);

    format_size(heap_caps_get_total_size(MALLOC_CAP_SPIRAM), buf, sizeof(buf));
    ESP_LOGI(TAG, "  PSRAM total (heap_caps): %s", buf);
    format_size(heap_caps_get_free_size(MALLOC_CAP_SPIRAM), buf, sizeof(buf));
    ESP_LOGI(TAG, "  PSRAM free now:          %s", buf);
    format_size(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM), buf, sizeof(buf));
    ESP_LOGI(TAG, "  PSRAM min free ever:     %s", buf);
    format_size(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), buf, sizeof(buf));
    ESP_LOGI(TAG, "  PSRAM largest block:     %s", buf);
}

static void print_partition_table(void) {
    char size_buf[64];
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "PARTITION TABLE  (live — read from the chip, offset 0x8000)");
    ESP_LOGI(TAG, "--------------------------------------");
    ESP_LOGI(TAG, "  %-10s %-6s %-9s %-10s %s", "Name", "Type", "SubType", "Offset", "Size");

    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        format_size(p->size, size_buf, sizeof(size_buf));
        ESP_LOGI(TAG, "  %-10s %-6s 0x%02x      0x%06lx   %s",
                 p->label, (p->type == ESP_PARTITION_TYPE_APP) ? "app" : "data",
                 p->subtype, (unsigned long)p->address, size_buf);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
}

static void print_nvs_info(void) {
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "NVS INFO  (live)");
    ESP_LOGI(TAG, "--------------------------------------");
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        ESP_LOGI(TAG, "  Used entries:  %d", stats.used_entries);
        ESP_LOGI(TAG, "  Free entries:  %d", stats.free_entries);
        ESP_LOGI(TAG, "  Total entries: %d", stats.total_entries);
    } else {
        ESP_LOGW(TAG, "  NVS stats unavailable");
    }
}

static void storage_mount_once(void) {
    // Only mount here for the report if the trips API module hasn't
    // already mounted it (avoids a duplicate/conflicting mount attempt).
#if ENABLE_TRIPS_API
    s_spiffs_mounted = true; // rest_api_storage_init() (called from app_main below) owns the mount
    return;
#else
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 10,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK) {
        s_spiffs_mounted = true;
    } else {
        ESP_LOGW(TAG, "No 'storage' SPIFFS partition found / mount failed (%s)", esp_err_to_name(ret));
    }
#endif
}

static void print_storage_info(void) {
    char buf[64];
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "STORAGE  (SPIFFS 'storage' partition — live)");
    ESP_LOGI(TAG, "--------------------------------------");

    if (!s_spiffs_mounted) {
        ESP_LOGW(TAG, "  Not mounted");
        return;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    format_size(total, buf, sizeof(buf));
    ESP_LOGI(TAG, "  Total: %s", buf);
    format_size(used, buf, sizeof(buf));
    ESP_LOGI(TAG, "  Used:  %s", buf);
    format_size(total - used, buf, sizeof(buf));
    ESP_LOGI(TAG, "  Free:  %s", buf);
}

static void print_full_report(void) {
    printf("\n\n");
    ESP_LOGI(TAG, "########################################");
    ESP_LOGI(TAG, "#   ESP32-S3 CHIP / MEMORY / STORAGE REPORT  #");
    ESP_LOGI(TAG, "########################################");

    version_print_banner();
    print_identity_banner();
    print_chip_info();
    print_flash_info();
    print_heap_info();
    print_partition_table();
    print_nvs_info();
    print_storage_info();

    ESP_LOGI(TAG, "########################################");
    printf("\n");
}

// ═══════════════════════════════════════════════════════════════
//  WIFI — starts whenever ENABLE_WIFI (config.h, via NETWORK_NEEDED) is
//  on. Unlike the original chip_info_test, this is now INDEPENDENT of
//  ENABLE_OTA/ENABLE_TRIPS_API/ENABLE_REMOTE_CONFIG — toggling any of
//  those can never silently take WiFi down too.
// ═══════════════════════════════════════════════════════════════
#if NETWORK_NEEDED
// doc 169 — the OLD version of this function hardcoded one SSID/password
// from config.h and blocked boot FOREVER (xEventGroupWaitBits(...,
// portMAX_DELAY)) until that exact network was found — not viable in
// production, where the deployed vehicle's WiFi (if any) isn't known at
// build time and a missing/out-of-range network must never hang the
// device. All of the SSID/password storage, connect/disconnect
// decision-making, and the event handlers that used to live here have
// moved to wifi_sta.c (NVS-backed, runtime-configurable via the "wifi
// ..." serial commands / the PC GUI's WiFi screen) — this function now
// only brings the WiFi DRIVER up (always needed, cellular/hotspot/etc.
// all assume esp_netif+esp_wifi exist) and hands off to
// app_wifi_init(), which does a single BOUNDED connect attempt and
// returns either way so boot always proceeds.
static void wifi_driver_bringup(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Doc 165 §hotspot investigation: SoftAP clients (hotspot_ap.c) were
    // seen failing the WPA2 4-way handshake repeatedly ("m f auth"/
    // "m f null" retries -> "deauth reason:15" = handshake timeout, in a
    // loop, never actually connecting). ESP-IDF's default WiFi power-save
    // (modem-sleep) periodically pauses the radio between DTIM beacons —
    // a well-known cause of exactly this symptom on ESP32 APSTA mode,
    // since a client's EAPOL handshake frames can arrive while the radio
    // is asleep and get delayed past the handshake's own timeout. This
    // device is always mains/vehicle powered (no battery-saving need),
    // so there's no downside to disabling it outright.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    app_wifi_init();
}
#endif // NETWORK_NEEDED

// ═══════════════════════════════════════════════════════════════
//  SNTP TIME SYNC — the TaxiMeter backend needs real wall-clock time:
//  tariff time-of-day/day-of-week windows and the public-holiday
//  22:00-night-before rule (reference_data.c) both compare against
//  time(NULL)/gmtime_r(). Runs once, right after WiFi connects, before
//  any HTTPS call that might need a valid clock for TLS certificate
//  validation.
// ═══════════════════════════════════════════════════════════════
#if NETWORK_NEEDED
// doc 182 10.8: the old loop condition was `while (status == RESET)` —
// but SNTP moves out of RESET into IN_PROGRESS almost the instant the
// first request goes out (well under 1s after esp_sntp_init()), so the
// loop was exiting there, not waiting for the real answer. It then
// logged "FAILED" regardless of whether the sync went on to actually
// succeed a moment later — a near-guaranteed false negative, confirmed
// against a real boot log (doc 182 §5: WiFi connected at 4863ms,
// "FAILED" logged at 6299ms — ~14 iterations of the 100ms loop, not the
// full 200/20s budget). Fixed to wait for the REAL terminal state
// (either COMPLETED or, per esp_sntp's own reset-on-failure behavior,
// back to RESET after a failed attempt) — see doc 182 §5's own honest
// caveat: this fixes the LOGGING; it doesn't change how SNTP itself
// behaves. s_ntp_synced_at_boot is exposed via a "time" serial command
// so the wall clock's real state can be checked on demand, not just
// inferred from one boot-time log line.
static bool s_ntp_synced_at_boot = false;

static void time_sync_init(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    // Waits through BOTH RESET (before the first request is even sent —
    // this is the phase the old `while (status == RESET)` condition
    // exited on immediately, the actual bug) and IN_PROGRESS (waiting
    // for the server's response), stopping only on a genuine terminal
    // result: COMPLETED, or the budget running out.
    int retry = 0;
    while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && ++retry < 200) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_ntp_synced_at_boot = (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
    ESP_LOGI(TAG, "NTP sync: %s",
        s_ntp_synced_at_boot ? "OK \xE2\x9C\x93" : "FAILED (tariff time-of-day lookups may be wrong until it syncs; try 'time' to check current status)");
}

// doc 182 10.8 — lets you actually verify the clock's real state
// instead of trusting a single boot-time log line (SNTP keeps syncing
// in the background in SNTP_OPMODE_POLL after boot, so "FAILED at boot"
// doesn't necessarily mean "still wrong now").
static bool time_process_command(const char *line) {
    if (!line || strcmp(line, "time") != 0) return false;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm_now);
    sntp_sync_status_t st = esp_sntp_get_sync_status();
    ESP_LOGI(TAG, "Wall clock: %s | epoch=%lld | SNTP status: %s | synced at boot: %s",
             buf, (long long)now,
             st == SNTP_SYNC_STATUS_COMPLETED ? "COMPLETED" :
             st == SNTP_SYNC_STATUS_IN_PROGRESS ? "IN_PROGRESS" : "RESET (never synced)",
             s_ntp_synced_at_boot ? "yes" : "no");
    return true;
}
#endif

// ═══════════════════════════════════════════════════════════════
//  "help" / "?" — top-level command list, grouped by category. Only
//  lists commands that actually exist in this build (ENABLE_* gated),
//  so it never advertises something that would print "Unknown
//  command". See docs/TestFunctionalities/esp32s3_board/
//  141_2026-07-25_serial_command_inventory_and_gui_gap_analysis.md for
//  the full reference (syntax, flows, GUI mapping).
// ═══════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════
//  AT<command> — raw AT command passthrough to the A7670E modem, no
//  wrapper needed (type "AT+CSQ" exactly as you would in any other
//  modem terminal). Forwarded to gps_client_send_raw_at(), which owns
//  the UART1 the modem lives on — see gps_backend_gnss.c for how it
//  stays safe to use even while a GNSS fix is actively streaming on the
//  same wire. See docs/TestFunctionalities/esp32s3_board/
//  141_2026-07-25_serial_command_inventory_and_gui_gap_analysis.md.
// ═══════════════════════════════════════════════════════════════
static void _handle_at_passthrough(const char *cmd) {
    char resp[512];
    ESP_LOGI(TAG, "AT> %s", cmd);
    bool ok = gps_client_send_raw_at(cmd, resp, sizeof(resp), 3000);
    printf("%s\n", resp);
    if (!ok) {
        ESP_LOGW(TAG, "AT command timed out or returned no clean OK/ERROR — see raw response above.");
    }
}

static void _print_help(void) {
    printf("\n=== esp32s3_board_test — command reference ===\n");
    printf("General:      help | ?  |  getinfo | info  |  getversion  |  reboot | restart\n");
    printf("Modem AT:     AT<command>  (raw passthrough, no wrapper — e.g. AT+CSQ, AT+COPS?)\n");
    printf("RAM:          ram info  |  ram test sram|psram|download|all\n");
    printf("Network:      net info  |  net test  |  net uplink wifi|cellular|auto  |  net status\n");
    printf("Cellular:     cell up  |  cell down  |  cell status  |  cell apn <apn>  |  cell ip\n");
    printf("Hotspot:      hotspot on|off  |  hotspot status  |  hotspot ssid <name>  |  hotspot passwd <old> <new>\n");
    printf("              hotspot clients  |  hotspot kick <mac>  |  hotspot reset-credentials <passcode>\n");
    printf("Diagnostics:  mem  |  store\n");
    printf("NVS:          nvs status  |  nvs company <id>  |  nvs product <name>\n");
    printf("Factory:      factory reset            (arms; passcode on the NEXT line)\n");
    printf("Setup:        setup network <ssid> <pass>  |  setup vehicle <plate>  |  setup info | setup help\n");
    printf("Auth:         auth login <username> <password>  |  auth logout  |  auth info | auth help\n");
    printf("Reference:    ref fetch  |  ref list  |  ref info | ref help\n");
    printf("Duty:         duty on  |  duty off  |  duty info\n");
    printf("Trip/Meter:   trip start  |  trip stop  |  trip pause  |  trip resume  |  trip extras  |  trip info | trip help\n");
    printf("Session:      session info  |  session clear\n");
    printf("Trips API:    api get|read|list|delete|info|help\n");
    printf("GPS:          gps info  |  gps source gnss|neo6m|inject  |  gps set <lat> <lon> <speed> [hdop] [sats]\n");
    printf("              gps gnss on|off|info|agps  |  gps neo6m on|off|info|start|stop|once|every\n");
#if ENABLE_OTA
    printf("OTA:          ota check  |  ota status\n");
#endif
#if ENABLE_MINI_COMMAND
    printf("Game:         game  |  guess <n>\n");
#endif
#if ENABLE_REMOTE_CONFIG
    printf("Remote cfg:   config status  |  config check\n");
#endif
#if ENABLE_ADDITIONAL_WORK
    printf("SMS (sim):    sms <command text>   (bench-test simulator, recognized: 'sms reboot'/'sms restart')\n");
#endif
#if ENABLE_SMS
    printf("SMS (real):   smsc send <number> <message>  |  smsc list  |  smsc status  |  smsc help\n");
    printf("              incoming SMS commands: TAXI#STATUS | TAXI#LOCATE | TAXI#NET | TAXI#REBOOT <passcode>\n");
#endif
#if ENABLE_LLM
    printf("LLM:          llm run <prompt>  |  llm run <steps> <prompt>  |  llm help\n");
#endif
    printf("===============================================\n\n");
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMAND READER
// ═══════════════════════════════════════════════════════════════
static void serial_cmd_task(void *arg) {
    char line[96];
    int  pos = 0;

    printf("\nType 'help' for the full command list. Quick start: 'getinfo' | 'getversion' | 'ram info' | 'ram test sram|psram|download|all'"
           " | 'net info' | 'net test' | 'AT+CSQ' (any AT<command>, raw modem passthrough) | 'api help' | 'gps set|info' | 'setup help' | 'auth help' | 'ref help'"
           " | 'duty on|off|info' | 'trip help' | 'sync now|status' | 'directions test' | 'session info' | 'mem' | 'store' | 'time'"
           " | 'net uplink wifi|cellular|auto' | 'cell up|status' | 'hotspot on|off|status'"
#if ENABLE_SMS
           " | 'smsc send|list|status'"
#endif
#if ENABLE_OTA
           " | 'ota check' | 'ota status'"
#endif
#if ENABLE_MINI_COMMAND
           " | 'game'"
#endif
#if ENABLE_REMOTE_CONFIG
           " | 'config status' | 'config check'"
#endif
#if ENABLE_ADDITIONAL_WORK
           " | 'sms <command>'"
#endif
#if ENABLE_LLM
           " | 'llm run <prompt>'"
#endif
           " | 'factory reset' | 'nvs status'"
           "\n> ");

    while (1) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (pos > 0) {
                line[pos] = '\0';

                // Leading-whitespace-trimmed alias of `line` — only the AT
                // passthrough check needs this (a real AT command is
                // recognized purely by its "AT" prefix, so a stray leading
                // space shouldn't hide it); every other command below still
                // matches on `line` unchanged, same as before.
                const char *trimmed = line;
                while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

                // Checked FIRST, unconditionally — while "armed" (awaiting
                // its passcode), it must consume the very next line
                // regardless of what it looks like, even if it happens to
                // match another command's name.
                if (factory_reset_process_line(line)) {
                    // handled — either armed the prompt or consumed a passcode attempt
                } else if (strcasecmp(line, "help") == 0 || strcmp(line, "?") == 0) {
                    _print_help();
                } else if (strcasecmp(line, "reboot") == 0 || strcasecmp(line, "restart") == 0) {
                    printf("Rebooting...\n");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                } else if (strncasecmp(trimmed, "AT", 2) == 0) {
                    _handle_at_passthrough(trimmed);
                } else if (strcasecmp(line, "getinfo") == 0 || strcasecmp(line, "info") == 0) {
                    print_full_report();
                } else if (strcasecmp(line, "getversion") == 0) {
                    version_print_banner();
                } else if (ram_test_process_command(line)) {
                    // handled
                } else if (net_diag_process_command(line)) {
                    // handled — "net info" / "net test" (WiFi status + real internet check)
                } else if (gps_client_process_command(line)) {
                    // handled
                } else if (rest_api_storage_process_command(line)) {
                    // handled — "api ..." (trip fetch/storage)
                } else if (session_store_process_command(line)) {
                    // handled
                } else if (setup_client_process_command(line)) {
                    // handled
                } else if (auth_client_process_command(line)) {
                    // handled
                } else if (login_screen_process_command(line)) {
                    // handled — "login gate hard|soft|info" (doc 179 D1c)
                } else if (reference_data_process_command(line)) {
                    // handled
                } else if (duty_client_process_command(line)) {
                    // handled
                } else if (trip_manager_process_command(line)) {
                    // handled
                } else if (trip_sync_process_command(line)) {
                    // handled — "sync now" / "sync status"
                } else if (directions_client_process_command(line)) {
                    // handled — "directions test <lat1> <lon1> <lat2> <lon2>"
                } else if (app_wifi_process_command(line)) {
                    // handled — "wifi scan|connect|disconnect|autoconnect|status" (doc 169)
                } else if (cellular_ppp_process_command(line)) {
                    // handled — "cell up|down|status|apn|ip" (Phase 1, doc 155/158)
                } else if (hotspot_ap_process_command(line)) {
                    // handled — "hotspot on|off|status|ssid|passwd|clients|kick|reset-credentials" (Phase 1)
#if ENABLE_SMS
                } else if (sms_client_process_command(line)) {
                    // handled — "smsc send|list|status" (Phase 2, doc 155/159) — NOT "sms", see config.h's comment
#endif
                } else if (diag_process_command(line)) {
                    // handled — "mem" / "store" / "diag help"
#if NETWORK_NEEDED
                } else if (time_process_command(line)) {
                    // handled — "time" (doc 182 10.8 — real wall-clock/SNTP status, not inferred from one boot log line)
#endif
#if ENABLE_OTA
                } else if (ota_client_process_command(line)) {
                    // handled
#endif
#if ENABLE_MINI_COMMAND
                } else if (mini_command_process(line)) {
                    // handled
#endif
#if ENABLE_REMOTE_CONFIG
                } else if (remote_config_process_command(line)) {
                    // handled
#endif
#if ENABLE_ADDITIONAL_WORK
                } else if (additional_work_process_command(line)) {
                    // handled
#endif
#if ENABLE_LLM
                } else if (llm_runner_process_command(line)) {
                    // handled
#endif
                } else if (nvs_state_process_command(line)) {
                    // handled
                } else {
                    printf("Unknown command '%s'.\n", line);
                }
                pos = 0;
                printf("> ");
            }
        } else if (ch == '\b' || ch == 127) {
            if (pos > 0) pos--;
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)ch;
            putchar(ch);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  LVGL TICK TIMER — provides lv_tick_inc() for animations
// ═══════════════════════════════════════════════════════════════
static void lv_tick_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LVGL_TICK_PERIOD_MS));
        lv_tick_inc(LVGL_TICK_PERIOD_MS);
    }
}

#if ENABLE_PSRAM_TASK_STACKS
// Phase 0 round 3 (config.h's ENABLE_PSRAM_TASK_STACKS comment has the
// full safety reasoning) — lv_tick_task's entire body is
// vTaskDelay()+lv_tick_inc(), never touches flash/NVS/SPIFFS, so its
// stack is safe to move to PSRAM entirely, freeing its internal-SRAM
// footprint. The TCB itself (StaticTask_t) must stay in internal SRAM
// regardless (a hard FreeRTOS/ESP-IDF requirement — only the stack
// buffer may be external) — it's tiny (~76 bytes), negligible.
static StaticTask_t s_lv_tick_tcb;
static StackType_t *s_lv_tick_stack = NULL;
#define LV_TICK_STACK_BYTES 4096
#endif

// ═══════════════════════════════════════════════════════════════
//  This timer's job is now just the SMS-reboot dialog courtesy + LVGL
//  mem-pool sampling below. Live meter values (doc 179 §5 restructure,
//  2026-08-06) are no longer relayed through here — meter_screen.c and
//  test_meter_dev.c each own a 1s lv_timer that reads
//  fare_calc_get_snapshot() directly, same self-contained pattern
//  network_screen.c/trip_screen.c already use. Still an LVGL timer
//  callback, not a separate task calling LVGL directly (doc 116).
// ═══════════════════════════════════════════════════════════════
// Phase 2 (doc 155/159) — SMS REBOOT interlock's GUI courtesy. Backend
// code (sms_commands.c) can never call LVGL directly (doc 116) — it
// just sets a plain flag/message; THIS callback, which already runs on
// the LVGL thread every 1s, is what actually shows the dialog. The
// forced-timeout reboot itself is guaranteed independently by
// sms_commands.c's own esp_timer, NOT by this poll — even if the
// display were somehow stuck, the reboot still happens.
static void _on_sms_reboot_confirm(void *user_data) {
    (void)user_data;
    sms_commands_reboot_now();
}

static void _dashboard_timer_cb(lv_timer_t *timer) {
    if (sms_commands_reboot_ui_pending()) {
        char msg[128];
        sms_commands_reboot_ui_consume(msg, sizeof(msg));
        confirm_dialog_show(lv_scr_act(), msg, _on_sms_reboot_confirm, NULL, NULL);
    }

    // Phase 0 (doc 155 §12.4): sample LVGL's own memory pool from HERE
    // specifically — this callback already runs on the LVGL thread, which
    // is the only place lv_mem_monitor() may legally be called. The "mem"
    // serial command then reads the cached result from its own task
    // without ever touching LVGL. Cheap (a few struct field copies), so
    // running it on the existing 1s dashboard tick costs nothing
    // measurable and needs no extra timer.
    ui_refresh_lvgl_mem_stats();
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_state_init();
    session_store_init();

    // Background worker (Trip FETCH + every TaxiMeter HTTPS call routed
    // via bg_worker_submit_fn()) — started FIRST, before anything else
    // touches the heap, so its one-time 8KB stack allocation can't fail
    // later once WiFi/display/SPIFFS have fragmented/consumed most of it.
    bg_worker_init();

    // Serial console — SAME reasoning as bg_worker_init() above, and
    // moved here 2026-07-30 after a real hardware incident: this task
    // used to be created near the very END of app_main() (after
    // display_init()/touch_init()/ui_init()/ram_test_init()), so its
    // 8KB stack request competed for internal-SRAM's LARGEST CONTIGUOUS
    // block against the LVGL pool (112KB, doc 160/161), the display's
    // DMA draw buffers, and everything else touched by then. On real
    // hardware this request failed — and because xTaskCreate()'s return
    // value was never checked, the failure was completely silent: no
    // crash, no log line, the rest of the system ran fine, but the
    // serial console simply never existed (confirmed: its own startup
    // banner never printed, and zero typed commands ever got a
    // response, in a log that otherwise ran cleanly for 200+ seconds —
    // see doc 161 §5). Creating it here, before WiFi/display/anything
    // else fragments the heap, is the same fix bg_worker_init() and
    // cellular_ppp_init() already apply for their own large one-time
    // allocations. The explicit pdPASS check below is new too — so if
    // this EVER fails again for any reason, it is loud, not silent.
    TaskHandle_t serial_cmd_handle = NULL;
    if (xTaskCreate(serial_cmd_task, "serial_cmd", 8192, NULL, 2, &serial_cmd_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(serial_cmd_task) FAILED — serial console will not respond to "
                      "any typed command this boot (see doc 161 §5). Likely out of contiguous "
                      "internal SRAM even this early in boot — check 'mem' isn't reachable either, "
                      "in which case this log line itself is the only diagnostic you'll get.");
    } else {
        diag_register_task(serial_cmd_handle, "serial_cmd");   // doc 184 §10.2 — see 'stacks'
    }

#if NETWORK_NEEDED
    wifi_driver_bringup();

    // ── PHASE 1 (doc 155/158): cellular + hotspot ──────────────────
    // Deliberately placed HERE — right after WiFi-STA connects, BEFORE
    // SNTP/provisioning/display/SPIFFS touch the heap further. This is
    // the SAME "grab the largest contiguous block while the heap is
    // least fragmented" discipline bg_worker_init() already follows
    // (its own header comment states the identical principle) — the
    // USB host driver (cellular_ppp_init()) needs a sizeable contiguous
    // internal-SRAM/DMA block, and this is the earliest point after the
    // event loop + esp_netif are actually available (both come from
    // wifi_init_and_wait() above).
    hotspot_nvs_init();
    cellular_ppp_init();
    hotspot_ap_init();
    net_manager_init();

    // SNTP + one-time-per-device provisioning (network passcode + vehicle
    // lookup) run HERE, immediately after WiFi connects and BEFORE
    // display/touch/UI/SPIFFS touch the heap — the heap-hungry TLS
    // handshake gets first pick of the largest free heap available all
    // boot, same reasoning bg_worker_init() above already follows.
    time_sync_init();
    setup_client_run_if_needed();
#endif

    storage_mount_once();
    rest_api_storage_init();

    // reference_data_init() must come AFTER rest_api_storage_init() —
    // it depends on SPIFFS already being mounted there.
    reference_data_init();
    gps_client_init();
    trip_manager_init();

#if ENABLE_SMS
    // Phase 2 (doc 155/159) — AFTER gps_client_init() (needs the GNSS
    // backend's UART1 + mutex already up) and trip_manager_init() (the
    // STATUS command reads live trip state).
    sms_client_init();
#endif

    // Automatic report — ONCE, at boot (unchanged behaviour from the
    // original diagnostic tool).
    print_full_report();

    // ── Display (ST7796S + LVGL) + Touch (FT6336U) + UI ─────────────
    // ESP_ERROR_CHECK — each of these already logs its own failure
    // reason; a hard failure here reboots rather than continuing into
    // a half-initialized LVGL state (same style already used for the
    // WiFi calls above).
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_LOGI(TAG, "Tap the screen — all taps logged to Serial Monitor");

#if ENABLE_OTA
    ota_client_init();
#endif
#if ENABLE_REMOTE_CONFIG
    remote_config_init();
#endif
#if ENABLE_ADDITIONAL_WORK
    additional_work_init();
#endif

    // Starts the periodic PSRAM health-check task — see ram_test.c.
    ram_test_init();

#if ENABLE_LLM
    // Mounts the dedicated `llm` SPIFFS partition only — the model itself
    // is loaded lazily on the first "llm run" command, not here (doc 125).
    llm_runner_init();
#endif

#if ENABLE_PSRAM_TASK_STACKS
    // PSRAM-backed stack — see this task's own comment above + config.h's
    // ENABLE_PSRAM_TASK_STACKS for why this specific task is safe to move.
    s_lv_tick_stack = (StackType_t *)heap_caps_malloc(LV_TICK_STACK_BYTES, MALLOC_CAP_SPIRAM);
    if (s_lv_tick_stack) {
        xTaskCreateStaticPinnedToCore(lv_tick_task, "lv_tick", LV_TICK_STACK_BYTES, NULL, 2,
                                       s_lv_tick_stack, &s_lv_tick_tcb, tskNO_AFFINITY);
        ESP_LOGI(TAG, "lv_tick task: stack on PSRAM (%d bytes, internal SRAM freed)", LV_TICK_STACK_BYTES);
    } else {
        ESP_LOGW(TAG, "lv_tick: PSRAM stack alloc failed — falling back to internal SRAM");
        xTaskCreate(lv_tick_task, "lv_tick", 2048, NULL, 2, NULL);
    }
#else
    xTaskCreate(lv_tick_task, "lv_tick", 2048, NULL, 2, NULL);
#endif

    // lv_timer_create(), NOT xTaskCreate() — see _dashboard_timer_cb's
    // own comment above for why this matters (LVGL thread-safety).
    lv_timer_create(_dashboard_timer_cb, 1000, NULL);

    ESP_LOGI(TAG, "READY — Touch UI Active ✓");

    // ── LVGL Handler Loop — app_main() intentionally no longer returns
    // once the display is active (same as esp32_display_taxi_3's own
    // app_main()) ──
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LVGL_TICK_PERIOD_MS));
        lv_timer_handler();
    }
}
