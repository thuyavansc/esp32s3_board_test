/**
 * app_main.c — esp32s3_board: ESP32-S3 N16R8V board bring-up diagnostic
 * firmware, ported from esp32_chip_info_test/main/app_main.c.
 *
 * Base behaviour unchanged: prints chip/flash/RAM/partition/NVS/SPIFFS
 * info once at boot, "getinfo"/"info" reprints it on demand. Changes vs.
 * the original for this project:
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
 *     pins only — see config.h and docs 111/112): bg_worker_init() early
 *     (before WiFi touches the heap), display_init()/touch_init()/
 *     ui_init() after WiFi connects, lv_tick_task + sim_task started,
 *     and app_main()'s own final loop is now the LVGL handler loop —
 *     app_main() intentionally no longer returns.
 *
 * ================================================================
 * SERIAL COMMANDS (available ones depend on which flags are ON)
 * ================================================================
 *   getinfo / info     Full chip/flash/RAM(SRAM+PSRAM)/partition/storage report
 *   getversion         Firmware version + build label + running partition
 *   ram info           Live SRAM+PSRAM totals/free/largest-block (no allocation)
 *   ram test sram|psram|all   Allocate/write/verify/free real-value RAM test
 *   ota check/status   [ENABLE_OTA, currently OFF]
 *   api get/read/list/delete/info/help  [ENABLE_TRIPS_API]  Trip fetch/storage
 *   game / guess <n>   [ENABLE_MINI_COMMAND, currently OFF]
 *   config status|check [ENABLE_REMOTE_CONFIG, currently OFF]
 *   sms <command>      [ENABLE_ADDITIONAL_WORK]
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
#include "lvgl.h"
#include "config.h"
#include "version.h"
#include "backend/ota_client.h"
#include "backend/trips_api.h"
#include "backend/mini_command.h"
#include "backend/factory_reset.h"
#include "backend/nvs_state.h"
#include "backend/remote_config.h"
#include "backend/additional_work.h"
#include "backend/ram_test.h"
#include "backend/bg_worker.h"
#include "display/display_driver.h"
#include "display/touch_driver.h"
#include "display/ui_main.h"

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
    s_spiffs_mounted = true; // trips_api_init() (called from app_main below) owns the mount
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
#define BIT_WIFI_CONNECTED BIT0
static EventGroupHandle_t s_wifi_events;

static void _wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, BIT_WIFI_CONNECTED);
        ESP_LOGW(TAG, "WiFi disconnected — reconnecting...");
        esp_wifi_connect();
    }
}

static void _ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, BIT_WIFI_CONNECTED);
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected — IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void wifi_init_and_wait(void) {
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, _ip_event, NULL);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi connecting to '%s' ...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_events, BIT_WIFI_CONNECTED, pdFALSE, pdFALSE, portMAX_DELAY);
}
#endif // NETWORK_NEEDED

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMAND READER
// ═══════════════════════════════════════════════════════════════
static void serial_cmd_task(void *arg) {
    char line[96];
    int  pos = 0;

    printf("\nType 'getinfo' | 'getversion' | 'ram info' | 'ram test sram|psram|download|all'"
#if ENABLE_OTA
           " | 'ota check' | 'ota status'"
#endif
#if ENABLE_TRIPS_API
           " | 'api help'"
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

                // Checked FIRST, unconditionally — while "armed" (awaiting
                // its passcode), it must consume the very next line
                // regardless of what it looks like, even if it happens to
                // match another command's name.
                if (factory_reset_process_line(line)) {
                    // handled — either armed the prompt or consumed a passcode attempt
                } else if (strcasecmp(line, "getinfo") == 0 || strcasecmp(line, "info") == 0) {
                    print_full_report();
                } else if (strcasecmp(line, "getversion") == 0) {
                    version_print_banner();
                } else if (ram_test_process_command(line)) {
                    // handled
#if ENABLE_OTA
                } else if (ota_client_process_command(line)) {
                    // handled
#endif
#if ENABLE_TRIPS_API
                } else if (trips_api_process_command(line)) {
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

// ═══════════════════════════════════════════════════════════════
//  DASHBOARD DEMO VALUES — simulated speed/distance/fare, same as
//  the source project (esp32_display_taxi_3): this project has no
//  real speed sensor/odometer, so the Dashboard screen shows moving
//  demo values to prove the display+UI actually works end-to-end.
// ═══════════════════════════════════════════════════════════════
static void sim_task(void *arg) {
    double speed = 0.0, distance = 0.0, fare;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        speed += 2.0;
        if (speed > 55.0) speed = 0.0;
        distance += speed / 3600.0;
        fare = UI_DEFAULT_FLAG_FALL + distance * UI_DEFAULT_FARE_RATE;
        ui_update_dashboard(speed, distance, fare);
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    nvs_state_init();

    // Background worker (Trip FETCH) — started FIRST, before anything
    // else touches the heap, so its one-time 8KB stack allocation can't
    // fail later once WiFi/display/SPIFFS have fragmented/consumed most
    // of it (same reasoning as esp32_display_taxi_3's own app_main()).
    bg_worker_init();

#if NETWORK_NEEDED
    wifi_init_and_wait();
#endif

    storage_mount_once();
#if ENABLE_TRIPS_API
    trips_api_init();
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

    // Raised 4096 -> 8192: this task's own command buffer plus every
    // module's process_command() call chain (trips_api/ota/remote_config/
    // additional_work all get a turn on this same stack) left too little
    // headroom at 4096 on this larger, more heavily-flagged codebase.
    xTaskCreate(serial_cmd_task, "serial_cmd", 8192, NULL, 2, NULL);

    xTaskCreate(lv_tick_task, "lv_tick", 2048, NULL, 2, NULL);
    xTaskCreate(sim_task, "sim", 3072, NULL, 1, NULL);

    ESP_LOGI(TAG, "READY — Touch UI Active ✓");

    // ── LVGL Handler Loop — app_main() intentionally no longer returns
    // once the display is active (same as esp32_display_taxi_3's own
    // app_main()) ──
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LVGL_TICK_PERIOD_MS));
        lv_timer_handler();
    }
}
