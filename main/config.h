#pragma once
// ================================================================
// config.h — esp32s3_board: ESP32-S3 N16R8V board bring-up firmware
//
// Ported from esp32_chip_info_test/main/config.h. Same one-codebase,
// feature-flagged pattern — OTA and Remote-Config modules are fully
// present (not deleted) but switched OFF here, so this build stays a
// focused chip/flash/SRAM/PSRAM diagnostic tool. Flip either flag back
// to 1 later with no code changes needed elsewhere.
//
// Real hardware this targets: ESP32-S3R8, 16MB flash, 8MB Octal PSRAM
// (boards/esp32-s3-devkitc-1-n16r8v.json) — NOT the Waveshare
// ESP32-S3-A7670E-4G taxi-meter board (docs/TestFunctionalities/
// 0_2026-07-04_project_master_context.md, 5_.../6_...) — that board has
// an A7670E 4G modem + DIP switches this devkit does not have.
// ================================================================
#include "version.h" // FW_PROJECT_NAME, used by OTA_DEFAULT_PRODUCT_NAME below

// ── Feature flags ────────────────────────────────────────────────
#define ENABLE_OTA             0   // OTA check-in + esp_https_ota download — CODE KEPT, switched OFF
#define ENABLE_TRIPS_API       1   // HTTPS trip fetch + SPIFFS JSON storage
#define ENABLE_MINI_COMMAND    0   // extra serial "game" command (v4 demo) — unused here
#define ENABLE_REMOTE_CONFIG   0   // taxiNumber/smsNumber/emergencyContactNumber/serverUrl — CODE KEPT, switched OFF
#define ENABLE_ADDITIONAL_WORK 1   // catch-all for small standalone requirements — see additional_work.c
#define ENABLE_LLM              1   // TinyLlama-260K local inference (serial console only) — see llm/llm_runner.c, doc 123/125

// ── WiFi — independent of every other feature flag above ────────
// WiFi must connect regardless of whether OTA/Trips/Remote-Config are on
// or off (previously WiFi only connected when one of those needed it —
// changed here on purpose so toggling any of them can never silently
// take WiFi down too).
#define ENABLE_WIFI     1
#define NETWORK_NEEDED  ENABLE_WIFI

#define WIFI_SSID   "TWHSP"
#define WIFI_PASS   "TollWirelessWPA2"

// ── Factory reset safety passcode ──────────────────────────────
// Type "factory reset" then this passcode on the very next line in the
// Serial Monitor to force the boot partition back to `factory` and
// reboot immediately — see factory_reset.c. Always compiled in
// regardless of ENABLE_OTA/ENABLE_TRIPS_API/ENABLE_MINI_COMMAND — this
// is the recovery path FOR when one of them breaks something.
#define FACTORY_RESET_PASSCODE  "1010"

// ================================================================
// OTA — only compiled/used when ENABLE_OTA=1. Kept fully intact
// (unchanged from esp32_chip_info_test) so flipping ENABLE_OTA back to
// 1 later works immediately — nothing here needed to be deleted or
// rewritten to "disable" OTA; the ENABLE_OTA flag above is the only
// switch that matters.
// ================================================================
#define OTA_SERVER_MODE   0

#if OTA_SERVER_MODE == 1
  // REAL remote company server — HTTPS, standard port 443.
  #define OTA_SERVER_HOST   "mydevices.myweb.net.au"
  #define OTA_SERVER_PORT   443
  #define OTA_USE_HTTPS     1
  #define OTA_INSECURE_SKIP_CERT_VERIFY   0
#elif OTA_SERVER_MODE == 2
  #define OTA_SERVER_HOST   "192.168.8.168"
  #define OTA_SERVER_PORT   5273
  #define OTA_USE_HTTPS     0
  #define OTA_INSECURE_SKIP_CERT_VERIFY   0
#elif OTA_SERVER_MODE == 3
  #define OTA_SERVER_HOST   "192.168.8.168"
  #define OTA_SERVER_PORT   7273
  #define OTA_USE_HTTPS     1
  #define OTA_INSECURE_SKIP_CERT_VERIFY   1
#elif OTA_SERVER_MODE == 4
  #define OTA_SERVER_HOST   "ota-update-server-rem6xgeeb-mark0.vercel.app"
  #define OTA_SERVER_PORT   443
  #define OTA_USE_HTTPS     1
  #define OTA_INSECURE_SKIP_CERT_VERIFY   0
#else
  // LOCAL SvelteKit mirror (mode 0, default/fallback) — plain HTTP.
  #define OTA_SERVER_HOST   "192.168.8.168"
  #define OTA_SERVER_PORT   3000
  #define OTA_USE_HTTPS     0
  #define OTA_INSECURE_SKIP_CERT_VERIFY   0
#endif

#define OTA_FALLBACK_LOCAL_TEST_ENABLED   0
#define OTA_FALLBACK_LOCAL_HOST           "192.168.8.168"
#define OTA_FALLBACK_LOCAL_PORT           3000
#define OTA_FALLBACK_LOCAL_USE_HTTPS      0

#define OTA_MANIFEST_PATH   "/api/Esp32Ota/Manifest"
#define OTA_REPORT_PATH     "/api/Esp32Ota/Report"
#define OTA_HTTP_TIMEOUT_MS 15000

#define OTA_HTTP_BUFFER_SIZE   4096
#define OTA_HTTP_USER_AGENT    "ESP32-OTA-Client/1.0"

#define OTA_MANIFEST_SEND_FULL_PARAMS   0
#define OTA_CHECK_INTERVAL_S   30

#define OTA_MIN_SUPPORTED_MAJOR  1
#define OTA_MIN_SUPPORTED_MINOR  0

#define OTA_MAX_RETRIES_PER_VERSION   3

#define OTA_MANIFEST_RETRY_COUNT   3
#define OTA_DOWNLOAD_RETRY_COUNT   3
#define OTA_RETRY_DELAY_MS         1500

#define OTA_CHECK_BACKOFF_MAX_MULTIPLIER  8
#define OTA_CHECK_JITTER_PERCENT      20

#define VERSION_DISPLAY_LEGACY_SIXFIELD   0

#define OTA_DEFAULT_COMPANY_ID     "default-company"
#define OTA_DEFAULT_PRODUCT_NAME   FW_PROJECT_NAME

// ================================================================
// REMOTE CONFIGURATION — only compiled/used when ENABLE_REMOTE_CONFIG=1.
// Kept fully intact, same reasoning as the OTA block above.
// ================================================================
#define REMOTE_CONFIG_PATH                "/api/Esp32Config/RemoteConfig"
#define REMOTE_CONFIG_HTTP_TIMEOUT_MS      15000
#define REMOTE_CONFIG_RETRY_COUNT          3

#define REMOTE_CONFIG_CHECK_INTERVAL_S      60

#define REMOTE_CONFIG_DEFAULT_TAXI_NUMBER       ""
#define REMOTE_CONFIG_DEFAULT_SMS_NUMBER        ""
#define REMOTE_CONFIG_DEFAULT_EMERGENCY_NUMBER  ""

#define ENABLE_SERVER_URL_OVERRIDE   1

// ================================================================
// TRIPS API — only compiled/used when ENABLE_TRIPS_API=1.
// ================================================================
#define TRIPS_API_HOST         "mytaxis.softclient.com.au"
#define TRIPS_API_PATH         "/taxis-api/api/Trips"
#define TRIPS_DEFAULT_ID       12772
#define TRIPS_HTTP_TIMEOUT_MS  30000
#define TRIPS_BUFFER_SIZE      4096
#define STORAGE_DIR            "/store"
#define STORAGE_MAX_FILES      100

// ── Trips-API-specific auth (added for esp32s3_board) ───────────
// Deliberately named TRIPS_API_* (not a generic "AUTH_TOKEN") — this
// token is scoped to ONLY this one GET endpoint (TRIPS_API_HOST +
// TRIPS_API_PATH). It is NOT a general/shared credential: OTA's
// Manifest/Report calls and remote-config's own endpoint send no auth
// at all today, and any future API module that needs its own token
// should define its own <MODULE>_AUTH_ENABLED / <MODULE>_AUTH_TOKEN
// pair here rather than reusing this one. Stored in config.h (compiled
// in), NOT NVS — this is a build-time credential, not a per-device
// runtime value.
//
// ⚠ The token below is the same one already used in this repo
// (esp32_wave_board_test/main/config.h) — decoded, it is a JWT valid
// ONLY 2026-07-08 12:09:30 UTC through 2026-07-09 12:09:30 UTC (a
// 24-hour window). It is therefore ALREADY EXPIRED as of this build
// (2026-07-23). Left here as a wired-up placeholder with
// TRIPS_API_AUTH_ENABLED=0 — replace the token string and flip this to
// 1 once you have a fresh one. Sending an expired token behaves
// identically to sending none (401 / "Job id not found in trips."), so
// leaving it at 0 for now costs nothing.
#define TRIPS_API_AUTH_ENABLED  0        // 0 = OFF (no Authorization header), 1 = ON
#define TRIPS_API_AUTH_TOKEN \
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJodHRwOi8vc2NoZW1hcy54bWxzb2FwLm9yZy93cy8yMDA1LzA1L2lkZW50aXR5L2NsYWltcy9uYW1lIjoiMTEwMDEiLCJqdGkiOiI2YzQ5ZTI4OS01Y2MxLTQyYWQtYWY1Zi0yZDY3ZTk2Mjg4ZjQiLCJodHRwOi8vc2NoZW1hcy54bWxzb2FwLm9yZy93cy8yMDA1LzA1L2lkZW50aXR5L2NsYWltcy9uYW1laWRlbnRpZmllciI6IjVlZDBiZDU3LWFkOTktNDNkMS04ZDk3LWYzZjRkMTAwZTE2YyIsIk5ldHdvcmsiOiIyIiwiaHR0cDovL3NjaGVtYXMubWljcm9zb2Z0LmNvbS93cy8yMDA4LzA2L2lkZW50aXR5L2NsYWltcy9yb2xlIjoiQWRtaW4iLCJuYmYiOjE3ODM1MTI1NzAsImV4cCI6MTc4MzU5ODk3MCwiaXNzIjoiaHR0cDovL2xvY2FsaG9zdDo1MDgzIiwiYXVkIjoiaHR0cDovL2xvY2FsaG9zdDo1MDgzIn0.X6z8mZ1piSwQjA7XDFeCh4cj8Z0eB_O2gBpqb1iD5NA"

// ================================================================
// PSRAM DOWNLOAD TEST — real network download, buffered ENTIRELY in
// PSRAM (not streamed to flash/SPIFFS like trips_api.c/ota_client.c
// do), then verified by size + SHA-256 — psram_download_test.c/h, new
// for esp32s3_board. This is the "large, real, network-driven PSRAM
// allocation" test, as opposed to ram_test.c's synthetic pattern-fill
// test.
//
// URL/SHA-256 below are copied VERBATIM from a real manifest response
// already returned by this project's own existing OTA test
// infrastructure (the Vercel target ota_client.c already knows how to
// reach via OTA_SERVER_MODE=4) — GET
// https://ota-update-server-rem6xgeeb-mark0.vercel.app/api/Esp32Ota/Manifest?companyId=30
// returned this exact "url"/"sha256"/"sizeBytes" — nothing here was
// invented; it's an existing, already-reachable test asset.
// ================================================================
#define PSRAM_DL_TEST_URL     "https://ota-update-server-rem6xgeeb-mark0.vercel.app/Resources/Esp32Ota/30/esp32_chip_info_test/test_2mb.bin"
#define PSRAM_DL_TEST_SHA256  "5647f05ec18958947d32874eeb788fa396a05d0bab7c1b71f112ceb7e9b31eee"
#define PSRAM_DL_TEST_HTTP_TIMEOUT_MS  30000

// The manifest reports sizeBytes=2097152 (2MB) for the CURRENT file,
// but the underlying test asset may be swapped for a bigger one later
// (mentioned: up to ~3MB). This ceiling is a SAFETY LIMIT ONLY — the
// real buffer size actually allocated is always read LIVE from the
// HTTP response's own Content-Length header (never hardcoded to
// 2097152), so a bigger real file is handled automatically. This
// ceiling exists purely to refuse the allocation (cleanly, with a clear
// log message) if a response ever claims something unexpectedly huge —
// protects PSRAM from a runaway or malformed Content-Length instead of
// blindly trying to allocate whatever a server claims.
#define PSRAM_DL_TEST_MAX_BYTES  (4 * 1024 * 1024)   // 4MB safety ceiling

// ================================================================
// RAM TEST — SRAM + PSRAM confirmation (ram_test.c/h, new for
// esp32s3_board). See ram_test.h for the full behavior description.
// ================================================================
// SRAM (internal) is exercised on-demand only ("ram test sram") — this
// is the RAM that already "works as expected" per prior testing, so it
// doesn't need its own repeating background task. 32KB is a safe
// default test size: large enough to be a meaningful integrity check,
// small enough not to itself starve internal SRAM (~320KB total, shared
// with WiFi/TLS) when run after WiFi has already connected.
#define RAM_TEST_SRAM_SIZE_BYTES        (32 * 1024)

// PSRAM is the RAM actually being confirmed here — 1MB is a meaningful
// exercise of an 8MB pool without taking long to fill/verify.
#define RAM_TEST_PSRAM_SIZE_BYTES       (1 * 1024 * 1024)

// The dedicated PSRAM health-check task (ram_test_init()) runs once
// shortly after boot (proves PSRAM at startup, once the heap has
// settled from WiFi/NVS/SPIFFS init), then repeats periodically so a
// PSRAM fault occurring later during long-running operation is also
// caught, unattended, in the serial log.
#define RAM_TEST_PSRAM_TASK_FIRST_RUN_DELAY_S   3
#define RAM_TEST_PSRAM_TASK_INTERVAL_S          300   // 5 minutes

// ================================================================
// LLM — TinyLlama-260K local inference, ported from
// https://github.com/DaveBben/esp32-llm (llm.c/llm.h only — that repo's
// own main.c also drives a separate OLED via u8g2; NOT ported, not
// needed, not wanted — see doc 125). Serial-console-only feature:
// input/output both go through serial_cmd_task's existing UART path via
// a new "llm run <prompt>" command. Zero coupling with this project's
// own ST7796S/LVGL display code — see doc 123 §7a / doc 125.
//
// Model + tokenizer live in their OWN dedicated SPIFFS partition (`llm`,
// partitions_16mb.csv) rather than the existing `storage` partition —
// packaging a SPIFFS image wholesale-overwrites its target partition,
// which would destroy any trip data `storage` has accumulated at
// runtime if the two were combined.
// ================================================================
#define LLM_MOUNT_POINT      "/llm"
#define LLM_MODEL_PATH       "/llm/stories260K.bin"
#define LLM_TOKENIZER_PATH   "/llm/tok512.bin"
#define LLM_DEFAULT_STEPS    256     // ~13-14s at ~19 tok/s (doc 123) — override with "llm run <n> <prompt>"
// TEMPORARY diagnostic (doc 131/133) — 0.0f routes sample() through
// sample_argmax() (greedy) instead of sample_topp() (nucleus sampling),
// to test whether the token-511/hair-space collapse is in the logits
// computation itself (would still collapse) or in sample_topp()'s own
// fallback path (greedy would differ). Revert to 1.0f once diagnosed.
#define LLM_TEMPERATURE      0.0f
#define LLM_TOPP             0.9f

// ================================================================
// DISPLAY — Waveshare 3.5" Capacitive Touch LCD (ST7796S + FT6336U),
// 320x480, ported from esp32_display_taxi_3 (which targets classic
// ESP32 WROOM). ESP32-S3-ONLY pins below — no WROOM block at all
// (per instruction: remove, don't just disable/comment).
//
// Pin values are NOT esp32_display_taxi_3's own commented-out S3 block
// (that one was never actually tested — that project only ever builds
// for WROOM). These are the values already confirmed on a REAL
// ESP32-S3-N16R8V board with this exact display in this repo's sibling
// project esp32_wave_board_test — proven hardware over an untested
// guess. That's also why TOUCH_INT/TOUCH_RST are 18/8, not 17/16 as
// some docs suggest — 17/16 are free on THIS project (no GPS module),
// but 18/8 are what's actually wired/working on the real board.
// ================================================================
#define LCD_SPI_HOST    SPI3_HOST
#define LCD_MOSI        42
#define LCD_MISO        -1
#define LCD_SCLK        41
#define LCD_SD_CS       38   // TF-card slot CS, shares the SPI bus — MUST be held HIGH (display_driver.c does this)
#define LCD_CS          39
#define LCD_DC          40
#define LCD_RST         45   // strapping pin (VDD_SPI) — safe as GPIO after boot
#define LCD_BL          6

#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)  // 40 MHz SPI clock
#define LCD_BK_LIGHT_ON     1
#define LCD_BK_LIGHT_OFF    0

// ── FT6336U Touch (I2C) ────────────────────────────────────────
#define TOUCH_I2C_PORT    I2C_NUM_0
#define TOUCH_I2C_SDA     15
#define TOUCH_I2C_SCL     7
#define TOUCH_I2C_ADDR    0x38   // FT6336U 7-bit I2C address
#define TOUCH_INT         18
#define TOUCH_RST         8

// ── LVGL ────────────────────────────────────────────────────────
// 5% of screen rows per draw buffer (not a full 320x480 framebuffer) —
// same proven trade-off as the sibling display projects in this repo:
// keeps the two DMA draw buffers (~15KB each, internal SRAM — see doc
// 111/112 for why these can't be PSRAM) small, at the cost of slightly
// more flush cycles per full-screen redraw (negligible on this size
// display).
#define LVGL_BUF_SIZE_PCT  5
#define LVGL_TICK_PERIOD_MS 5

// ── UI Defaults ─────────────────────────────────────────────────
#define UI_DEFAULT_FARE_RATE   2.50   // $ per km
#define UI_DEFAULT_FLAG_FALL   3.80   // Base fare
