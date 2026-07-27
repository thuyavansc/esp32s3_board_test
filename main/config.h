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
// ⚠ PHASE 0 CHANGE (2026-07-26, doc 155 §12.4): 1 -> 0.
// The LLM is a demo/experiment feature, not taxi-meter functionality,
// and it is the easiest internal-SRAM lever to pull before adding the
// cellular/hotspot network stack. Two distinct savings:
//   1. Boot-time: llm_runner_init() mounts the dedicated `llm` SPIFFS
//      partition, whose cache/metadata buffers are internal SRAM.
//   2. Far more important — RISK: the model itself loads lazily on the
//      first "llm run" and pulls ~1MB (observed: PSRAM lowest-ever
//      dropped ~1MB in the captured session) plus internal task stack.
//      With USB-host + SoftAP + PPP running, an ad-hoc "llm run" could
//      starve the network stack mid-trip. Removing the command removes
//      that failure mode entirely.
// The `llm` flash partition and llm.c/llm_runner.c are DELIBERATELY left
// in place — flipping this back to 1 restores the feature with no
// partition migration and no code changes, exactly like ENABLE_OTA.
#define ENABLE_LLM              0   // TinyLlama-260K local inference (serial console only) — see llm/llm_runner.c, doc 123/125

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

// ── Trips-API-specific auth ───────────────────────────────────
// Deliberately named TRIPS_API_* (not a generic "AUTH_TOKEN") — this
// token is scoped to ONLY this one GET endpoint (TRIPS_API_HOST +
// TRIPS_API_PATH). It is NOT a general/shared credential: OTA's
// Manifest/Report calls and remote-config's own endpoint send no auth
// at all today. The TaxiMeter business API below (login/duty/tariffs/
// trip) uses its own Bearer token from session_store instead — see
// api_client.c. Stored in config.h (compiled in), NOT NVS — this is a
// build-time credential, not a per-device runtime value.
//
// Macro names (TRIPS_API_SEND_AUTH / TRIPS_API_BEARER_TOKEN) match what
// backend/taximeter/rest_api_storage.c actually reads.
//
// ⚠ The token below — decoded, it is a JWT valid ONLY 2026-07-08
// 12:09:30 UTC through 2026-07-09 12:09:30 UTC (a 24-hour window). It is
// therefore ALREADY EXPIRED as of this build. Left here as a wired-up
// placeholder with TRIPS_API_SEND_AUTH=0 — replace the token string and
// flip this to 1 once you have a fresh one. Sending an expired token
// behaves identically to sending none (401 / "Job id not found in
// trips."), so leaving it at 0 for now costs nothing.
#define TRIPS_API_SEND_AUTH  0        // 0 = OFF (no Authorization header), 1 = ON
#define TRIPS_API_BEARER_TOKEN \
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJodHRwOi8vc2NoZW1hcy54bWxzb2FwLm9yZy93cy8yMDA1LzA1L2lkZW50aXR5L2NsYWltcy9uYW1lIjoiMTEwMDEiLCJqdGkiOiI2YzQ5ZTI4OS01Y2MxLTQyYWQtYWY1Zi0yZDY3ZTk2Mjg4ZjQiLCJodHRwOi8vc2NoZW1hcy54bWxzb2FwLm9yZy93cy8yMDA1LzA1L2lkZW50aXR5L2NsYWltcy9uYW1laWRlbnRpZmllciI6IjVlZDBiZDU3LWFkOTktNDNkMS04ZDk3LWYzZjRkMTAwZTE2YyIsIk5ldHdvcmsiOiIyIiwiaHR0cDovL3NjaGVtYXMubWljcm9zb2Z0LmNvbS93cy8yMDA4LzA2L2lkZW50aXR5L2NsYWltcy9yb2xlIjoiQWRtaW4iLCJuYmYiOjE3ODM1MTI1NzAsImV4cCI6MTc4MzU5ODk3MCwiaXNzIjoiaHR0cDovL2xvY2FsaG9zdDo1MDgzIiwiYXVkIjoiaHR0cDovL2xvY2FsaG9zdDo1MDgzIn0.X6z8mZ1piSwQjA7XDFeCh4cj8Z0eB_O2gBpqb1iD5NA"

// ── Storage backend for rest_api_storage.c (1=LittleFS 2=SPIFFS(default)
//    3=SD 4=PSRAM 5=SRAM) — see backend/taximeter/rest_api_storage.h.
//    SPIFFS is this board's proven, board-tested option (matches the
//    'storage' partition already used everywhere else in this project).
#define STORAGE_BACKEND  2

// ================================================================
// TAXIMETER BUSINESS API — login, duty, tariffs, fare calc, trips.
// Ported from esp32_display_taxi_meter/main/backend/ — see
// docs/TestFunctionalities/esp32s3_board/
// 135_2026-07-25_fare_calc_and_backend_integration_plan.md for the full
// integration plan. Always compiled/active (this IS the taxi meter) —
// no ENABLE_* toggle, unlike OTA/remote-config, which are optional
// diagnostic extras.
// ================================================================
#define TAXIMETER_API_HOST        "mytaxis.softclient.com.au"   // same host as TRIPS_API_HOST
#define TAXIMETER_HTTP_TIMEOUT_MS  15000

// Endpoint paths
#define EP_NETWORK           "/devices-api/api/DevicePublic/Network"
#define EP_VEHICLE           "/devices-api/api/DevicePublic/Vehicle"
#define EP_LOGIN             "/taxis-api/api/Authentications/Login"
#define EP_LOGOUT            "/taxis-api/api/Authentications/Logout"
#define EP_DRIVER            "/taxis-api/api/Driver"
#define EP_ON_DUTY           "/taxis-api/api/DriverStatus/OnDuty"
#define EP_OFF_DUTY          "/taxis-api/api/DriverStatus/OffDuty"
#define EP_TARIFFS_FMT       "/taxis-api/api/VehicleType/%ld/Tarifs/v2"   // %ld = vehicleTypeId
#define EP_FIXED_FARES       "/taxis-api/api/FixedFares"
#define EP_SPECIAL_FARES     "/taxis-api/api/SpecialFare"
#define EP_PUBLIC_HOLIDAYS   "/devices-api/api/PublicHolidays"

// ── Trip-sync endpoints (driver variant — D1, doc 151 §7.1) ──────
// This device logs in with a username/password and holds a Bearer
// token (like Android's UserDetails.IS_DRIVER == true — doc 149
// §1.2), so it uses the SAME "Job/*" + "Trips" paths a driver's phone
// would, NOT the "DeviceJobs/*" + AppKey variant (that fork is for a
// headless meter provisioned with a per-device AppKey instead of a
// login — not our current provisioning model; revisit if/when an
// AppKey is ever issued for this device).
#define EP_ADD_JOB              "/taxis-api/api/Job/AddJobByDriver"
#define EP_TRIP_UPDATE          "/taxis-api/api/Trips"
#define EP_SAVE_JOB_FARES       "/taxis-api/api/Job/SaveJobFares"
#define EP_PICKUP               "/taxis-api/api/Job/Pickup"
#define EP_CHANGE_STATUS_FMT    "/taxis-api/api/Job/ChangeStatus/%d"   // %d = status ordinal

// Buffer for trip-sync JSON bodies/responses — bigger than
// API_SMALL_BUFFER_SIZE because the Trips-update payload carries the
// whole timeFrames[]/paths[] arrays (doc 149 §2.3), not the tiny
// login/duty-style bodies API_SMALL_BUFFER_SIZE was sized for.
#define TRIP_SYNC_BUFFER_SIZE    8192

// ================================================================
// DIRECTIONS (GraphHopper) — road-snapped distance, matching the
// Android reference's GetDirectionsUseCase/GraphHopperInstance (doc
// 149 §4.3, doc 150 §5/§7). D4 (doc 151 §7.1): the ESP32 does the same
// thing Android does — call a real routing service for any GPS gap
// bigger than DIRECTIONS_MIN_DISTANCE_M, instead of only ever billing
// the straight-line chord (which under-counts distance on a curving
// road).
//
// GRAPHHOPPER_API_KEY is a placeholder — get a real key from
// graphhopper.com (a free tier exists) before this is used against
// real trips; DIRECTIONS_ENABLED gates it off cleanly (falls back to
// straight-line haversine automatically, see directions_client.c) if
// no key is set yet, so the meter still works with an empty key.
// ================================================================
#define DIRECTIONS_ENABLED           1
#define DIRECTIONS_API_HOST          "graphhopper.com"
#define DIRECTIONS_API_PATH          "/api/1/route"
#define GRAPHHOPPER_API_KEY          ""   // ⚠ set a real key before relying on road-snapping in the field
#define DIRECTIONS_MIN_DISTANCE_M    250.0   // matches Android's GH_DIRECTIONS_MIN_DISTANCE (doc 150 §5/§12)
#define DIRECTIONS_HTTP_TIMEOUT_MS   10000
#define DIRECTIONS_BUFFER_SIZE       4096

// ================================================================
// TIME FRAMES — per-segment fare history (D3, doc 151 §7.1: full
// per-segment history, matching Android's TripTimeFrame table, doc
// 150 §0). Fixed-size arrays (not heap-allocated per row), same
// pattern as reference_data.h's REF_MAX_* — sized generously for a
// normal trip; "trip info"/logs warn (not crash) if a trip's real
// segment/point count ever exceeds these, exactly like ref data's own
// overflow policy. Raise these if that warning fires routinely.
// ================================================================
#define FARE_CALC_MAX_TIME_FRAMES        32   // one frame opens per GPS-active<->inactive flip or pause/resume
#define FARE_CALC_MAX_POINTS_PER_FRAME   40   // GPS points stored per frame, for the synced polyline

// Sent as the "App-Version" header on every TaxiMeter API call (see
// api_client.c's _perform()) — the server's Login endpoint rejects
// requests missing this header with HTTP 200 {"success":false,
// "message":"App version is outdated..."}, confirmed against a real
// login attempt. The real Android app sends its actual build's
// versionName (mytaxisv2/app/build.gradle.kts, format YY.MM.DD.build) —
// this is this firmware's own equivalent, dated to match this build.
#define TAXIMETER_APP_VERSION   "26.07.25.00"

// Device provisioning — network passcode + vehicle number are per-device,
// set-once values. No provisioning UI yet — set these once per physical
// device and reflash. Loaded into NVS by session_store on first boot
// only; "setup" serial commands can override at runtime for testing
// without a reflash.
#define SETUP_NETWORK_PASSCODE   "PBI8ZLOC"
#define SETUP_VEHICLE_NO         "Tu001"

// Driver login — TEMPORARY hardcoded test credentials, same "hardcode
// for the test bench, replace before field use" precedent as
// TRIPS_API_BEARER_TOKEN above. No login screen yet — use the "auth
// login" serial command to log in with these, or pass different
// credentials to that command directly.
#define AUTH_TEST_USERNAME       "12345"
#define AUTH_TEST_PASSWORD       "12345"

// Small, fixed-size buffers for the mostly-tiny JSON bodies these
// endpoints exchange (login/driver/network/vehicle/duty) — kept off the
// heap deliberately. Reference-data fetches (tariffs/fixed-rates/
// special-fares/public-holidays), which can be larger, stream to a
// SPIFFS file instead — see backend/taximeter/reference_data.c.
#define API_SMALL_BUFFER_SIZE     2048

// ================================================================
// GPS — TWO backends compiled in and runnable in PARALLEL (GNSS +
// NEO-6M), plus always-on serial injection. See
// backend/gps/gps_client.c and docs/TestFunctionalities/esp32s3_board/
// 139_2026-07-25_gnss_agps_implementation_research_and_plan.md /
// 140_2026-07-25_agps_how_it_actually_works_explained.md for the full
// research and design this is built from. Board identity + the A7670E
// UART pins were confirmed in doc 138.
// ================================================================
#define ENABLE_GPS_GNSS    1   // A7670E built-in GNSS over modem AT-UART1 (GPIO18/17) — DEFAULT real GPS
#define ENABLE_GPS_NEO6M   1   // external NEO-6M NMEA over UART2 — kept compiled, runnable alongside GNSS

// Which backend feeds fare_calc by default at boot — runtime-switchable
// via "gps source gnss|neo6m|inject" without a rebuild (gps_client.h's
// gps_source_t enum defines these values).
#define GPS_DEFAULT_ACTIVE_SOURCE   GPS_SRC_GNSS

// ── GNSS (A7670E) — UART1, the modem's own AT-command channel ──
// GPIO 18(TX)/17(RX) are confirmed, reserved, real hardware pins (docs
// 5/6/136/138) — do not reassign these for anything else.
#define GNSS_UART_NUM              UART_NUM_1
#define GNSS_UART_TX               18
#define GNSS_UART_RX               17
#define GNSS_UART_BAUD             115200

// AT+CGNSSPORTSWITCH argument — routes NMEA output to this UART. The two
// GNSS samples researched disagree on the first parameter (1,1 in the
// ESP-IDF sample; 0,1 in the Arduino sample) — defaulting to the
// ESP-IDF sample's value since that's the framework we're actually
// using; verify on real hardware (doc 139 §9 open item 1) and adjust
// here if NMEA doesn't start streaming.
#define GNSS_CGNSSPORTSWITCH_ARGS  "1,1"

// Some boards need the modem's PWRKEY pulsed to power on; the confirmed-
// working ESP-IDF sample for THIS chip does not do this (relies on the
// board's own 4G DIP switch + an AT-retry-until-OK loop) — OFF by
// default. Flip on + verify GNSS_PWRKEY_GPIO if your board needs it
// (doc 139 §9 open item 2).
#define ENABLE_GNSS_PWRKEY         0
#define GNSS_PWRKEY_GPIO           21

// ── A-GPS / SUPL — OFF until a data-enabled SIM is inserted (doc 140:
// "in the config add a control for that, later after i insert sim we
// will enable that"). The AT trigger sequence is fully implemented and
// gated behind this flag; flipping it to 1 (after inserting a SIM with
// a data plan) makes gps_backend_gnss.c send the SUPL/XTRA setup
// commands automatically at GNSS bring-up, with a precondition check
// (SIM present + network registered) that falls back cleanly to plain
// (non-assisted) GNSS if either isn't ready — see
// backend/gps/gps_backend_gnss.c.
#define ENABLE_AGPS                0
#define AGPS_SUPL_SERVER           "supl.google.com"
#define AGPS_SUPL_PORT             7276
#define AGPS_ENABLE_XTRA           1   // Qualcomm XTRA predicted-ephemeris assist
#define AGPS_ENABLE_SUPL           1   // standard OMA SUPL A-GPS
#define AGPS_ENABLE_HOTSTILL       1   // fast re-acquisition after a fix

// ── NEO-6M — UART2, an external module, free/unclaimed pins (doc 136) ──
// UART2 is the one hardware UART this board has left completely free
// (UART0 = PC console, UART1 = GNSS/modem AT-channel, both reserved —
// see doc 136 §1's full pin inventory, and doc 139 §4's note to verify
// GPIO 1/2 are physically accessible on this board before wiring a real
// module). Change these here (not in gps_backend_neo6m.c) if your
// physical NEO-6M wiring differs.
#define NEO6M_UART_NUM             UART_NUM_2
#define NEO6M_UART_TX              1     // ESP32 TX → NEO-6M RX (not used for reading)
#define NEO6M_UART_RX              2     // ESP32 RX ← NEO-6M TX
#define NEO6M_UART_BAUD            9600  // NEO-6M default baud rate

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
// Greedy decoding (0.0f) proved to reliably get stuck repeating one
// token on this tiny 260K model (doc 131/133) even after bypassing the
// confirmed sample_argmax() bug — a known weakness of always picking the
// single "best" token on such a small model. Real, varied output needs
// actual randomness — nucleus (top-p) sampling via a real temperature.
#define LLM_TEMPERATURE      1.0f
#define LLM_TOPP             0.9f

// Per-token diagnostic logging ([cfg]/[wgt]/[dbg]/[argmax]/[smp-final] —
// doc 131/133 debugging). OFF (0) by default so normal use just prints the
// generated sentence with no extra noise. Set to 1 to re-enable when
// debugging a generation issue.
#define LLM_VERBOSE_DEBUG    0

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
// guess.
//
// TOUCH_INT moved 18 -> 14 (2026-07-25): GPIO 18 is the A7670E modem's
// AT-command UART1 TX (docs 5/6/136/138) — confirmed working, real
// hardware, NEVER to be reused for anything else (same reason GPIO 17
// is off-limits too — that's the modem's UART1 RX). GPIO 14 has zero
// conflicts with the LCD SPI bus, touch I2C bus, either UART, native
// USB, the PSRAM/Flash range, or any strapping pin — see doc 138 §4 and
// the follow-up analysis in docs/TestFunctionalities/esp32s3_board/ for
// the full pin-by-pin check. The physical wire was moved to match.
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
#define TOUCH_INT         14  // moved from 18 — GPIO18 is the modem's UART1 TX, do not reuse (see comment above)
#define TOUCH_RST         8

// ── LVGL ────────────────────────────────────────────────────────
// Percentage of screen rows per draw buffer (not a full 320x480
// framebuffer) — keeps the two DMA draw buffers small. These MUST be
// internal, DMA-capable SRAM (doc 111/112, doc 144 §3: the SPI
// peripheral DMAs pixels straight out of them, and PSRAM cannot serve
// DMA) — so they are pure internal-SRAM cost with no PSRAM escape.
//
// ⚠ PHASE 0 CHANGE (2026-07-26, doc 155 §12.4): 5 -> 3.
//   At 5%: 320 x (480*5/100 = 24 rows) x 2 bytes = 15,360 B per buffer,
//          x2 buffers = ~30KB internal SRAM.
//   At 3%: 320 x (480*3/100 = 14 rows) x 2 bytes =  8,960 B per buffer,
//          x2 buffers = ~17.5KB internal SRAM.   → frees ~12KB
// Cost: more flush cycles per full-screen redraw. On a 320x480 SPI
// display at 40MHz this is a small, mostly-imperceptible refresh
// slowdown — an acceptable trade for 12KB of the scarcest resource on
// the board. If the UI feels sluggish after this, raise it back to 5
// (the display code reads this value; no other change needed).
#define LVGL_BUF_SIZE_PCT  3
#define LVGL_TICK_PERIOD_MS 5

// ── UI Defaults ─────────────────────────────────────────────────
#define UI_DEFAULT_FARE_RATE   2.50   // $ per km
#define UI_DEFAULT_FLAG_FALL   3.80   // Base fare
