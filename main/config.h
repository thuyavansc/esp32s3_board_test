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

// #define WIFI_SSID   "TWHSP"
// #define WIFI_PASS   "TollWirelessWPA2"

// #define WIFI_SSID   "404"
// #define WIFI_PASS   "supun404404"

// doc 169 — these two are now ONLY the first-boot seed for wifi_sta.c's
// NVS-backed config (backend/network/wifi/wifi_sta.c) — a brand-new
// device with no NVS entry yet starts from these values, but from then
// on the REAL, live SSID/password/auto-connect preference lives in NVS
// and is changed at runtime via "wifi connect <ssid> <password>" (serial)
// or the PC GUI's WiFi screen — never by re-flashing. Editing these two
// lines after first boot has NO effect on an already-provisioned device.
#define WIFI_SSID       "Mobitel 4G-489E"
#define WIFI_PASS       "NoMeansNo"

// Bounded — boot must NEVER hang forever waiting for one specific
// network (production requirement: the deployed vehicle's WiFi, if any,
// isn't known at build time and may not be in range). If this expires,
// boot continues anyway; wifi_sta.c's own disconnect-handler keeps
// retrying in the background for as long as auto-connect stays enabled.
#define WIFI_STA_CONNECT_TIMEOUT_MS      15000

// Whether to even ATTEMPT a WiFi connection at boot / after a drop.
// Default ON to match this project's existing dev/test workflow (a
// known SSID above) — a real production unit that relies on cellular as
// its primary uplink should set this to 0 (or just run
// "wifi autoconnect off" / "wifi disconnect" once, which persists the
// same way) so it never wastes time trying to join a network that isn't
// there. Runtime-togglable without a rebuild either way.
#define WIFI_STA_DEFAULT_AUTO_CONNECT    1

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

// doc 188: trip-history list — confirmed against the real Android source
// (features/trip_sync/TripApi.kt's getTripHistoryByDriver()), the driver
// variant of the same POST this device's own Job/AddJobByDriver etc.
// already use (D1, doc 151 §7.1 — this device holds a driver Bearer
// token, not an AppKey, so it's the "Job/*" path, not "DeviceJobs/*").
#define EP_JOB_SEARCH           "/taxis-api/api/Job/GetAllBySearch"

// Buffer for trip-sync JSON bodies/responses — bigger than
// API_SMALL_BUFFER_SIZE because the Trips-update payload carries the
// whole timeFrames[]/paths[] arrays (doc 149 §2.3), not the tiny
// login/duty-style bodies API_SMALL_BUFFER_SIZE was sized for.
#define TRIP_SYNC_BUFFER_SIZE    8192

// doc 188: SEPARATE, larger buffer for GET Job/GetAllBySearch's
// response only — a real captured response with a 10-item page (each a
// full JobDto: nested pickup/dropOff/address/customer/vehicle/driver
// objects) truncated at TRIP_SYNC_BUFFER_SIZE (8192), a real bug caught
// from device-monitor-260811-073051.log ("Response TRUNCATED at 8191
// bytes... response is not valid JSON"), not a guess. Kept as its own
// constant rather than just raising TRIP_SYNC_BUFFER_SIZE, since
// AddJob/Trips/SaveJobFares fire every ~60s during an active trip and
// don't need a buffer this large — only the History tab's fetch
// (opened on-demand, not on a tight loop) does. Sized with real
// headroom, not just "bumped until the one captured response fit" —
// PSRAM is abundant (8MB total, ~100KB used elsewhere per 'mem') so
// there's no reason to size this tightly.
#define TRIP_HISTORY_BUFFER_SIZE 40960

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
#define GRAPHHOPPER_API_KEY          "eebf0f9d-b636-488d-b84c-4676be0c0e1d"   // set 2026-08-06 (doc 179 D7) — real key, road-snapping/reconciliation now live
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
// TRIPS_API_BEARER_TOKEN above. Used by the "auth login" serial command
// AND (doc 179 Phase 2) as the login SCREEN's dev pre-fill, gated by
// LOGIN_PREFILL_DEV_CREDENTIALS below — one flag to flip before a real
// field deployment.
#define AUTH_TEST_USERNAME       "12345"
#define AUTH_TEST_PASSWORD       "12345"

// ================================================================
// LOGIN SCREEN (doc 179 §5/Phase 2, 2026-08-06)
// ================================================================
// D1(c): the absolute, non-overridable production switch. 1 = the
// login screen ALWAYS hard-gates (no "Skip" link exists at all,
// regardless of any runtime toggle — session_store_get_login_gate_hard()
// short-circuits on this before it ever looks at NVS). 0 = a dev build,
// where the gate defaults to soft but can be flipped to hard and back
// at RUNTIME (no rebuild) via "login gate hard|soft" or the Settings
// screen — see session_store_set_login_gate_hard(). Flip this to 1
// before a real field deployment; nothing else in the login flow needs
// to change.
#define BUILD_IS_PRODUCTION      0

// Pre-fills the login screen's username/password fields with
// AUTH_TEST_USERNAME/PASSWORD above, so a one-tap LOGIN works during
// development (your explicit ask). Independent of BUILD_IS_PRODUCTION
// on purpose — turn this off separately once real credentials matter,
// even in a dev build.
#define LOGIN_PREFILL_DEV_CREDENTIALS   1

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

// ⚠ TURNED OFF 2026-08-03 (doc 171): the external NEO-6M's UART2 sits on
// GPIO 1 + 2 (NEO6M_UART_TX/RX below), and the display re-map in this
// same pass had to claim those two pins — they are among only FIVE
// uncommitted GPIOs this board exposes (see the DISPLAY block for the
// full explanation). Leaving this at 1 would make the UART2 driver and
// the LCD SPI bus fight over the same physical pins.
//
// Nothing is actually lost: the A7670E's built-in GNSS above is the real
// GPS this project uses (GPS_DEFAULT_ACTIVE_SOURCE below), and it has
// been confirmed producing genuine fixes on this hardware (doc 170 §2).
// Every NEO-6M log line in every session so far reads "reads=0 [not
// active]" — the module was never physically connected.
//
// To use a NEO-6M again, it would have to move to a different interface
// entirely (there are no spare GPIOs left) — most realistically by
// giving up the camera instead of the TF slot.
#define ENABLE_GPS_NEO6M   0   // external NEO-6M NMEA over UART2 — OFF: its pins now belong to the display

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
//
// ⚠ DO NOT enable this without changing GNSS_PWRKEY_GPIO first — as of
// doc 174, GPIO 21 is the LCD's hardware reset line (LCD_RST). Pulsing it
// would reset the display controller mid-operation. There is no free pin
// left to move PWRKEY to either; this stayed at 21 only because the flag
// is 0 and the pin is therefore never touched. It has never been needed
// on this board (the 4G DIP switch powers the modem).
#define ENABLE_GNSS_PWRKEY         0
#define GNSS_PWRKEY_GPIO           21   // ⚠ COLLIDES WITH LCD_RST — see warning above

// ── A-GPS — gated behind this flag; a precondition check (SIM present +
// network registered) falls back cleanly to plain (non-assisted) GNSS
// if either isn't ready — see backend/gps/gps_backend_gnss.c.
//
// doc 180/182 Fix F(b) (2026-08-06): this used to send FOUR commands
// (AT+CGPSURL + AT+CGNSSCMD=10/20/30,1) that do not exist on this A76XX
// chip at all — confirmed against SIMCom's own 652-page AT Command
// Manual V1.09, not guessed (doc 180 §4). AGPS_SUPL_SERVER/_PORT/
// _ENABLE_XTRA/_ENABLE_SUPL/_ENABLE_HOTSTILL described a Qualcomm/
// SIM7600 feature set this chip doesn't have — deleted along with that
// dead code. The correct command is ONE line, no server to configure:
// AT+CAGPS (manual §24.2.15) — SIMCom hard-codes its own AGNSS server.
//
// ⚠ NOT YET FIELD-VERIFIED on this board (doc 180 §9 Block C, doc 182
// §11 item "F(b) blocked"): whether this exact firmware build
// (A011B05A7670M7_F) actually implements AT+CAGPS, and whether it can
// open its own socket while PPP holds the same modem's PDP context, are
// both still open questions. If either is false, AT+CAGPS returns
// ERROR/a failure code and this falls through to plain (non-assisted)
// GNSS automatically — same safe failure mode as before. This also
// SUBSTANTIALLY SHRINKS (does not eliminate) the PPP-dial collision
// window doc 182 §3 root-caused: 1 command instead of 12, and each
// retried only on the rare "no response at all" case, not on every
// clean ERROR the old code's 3x-retry-per-command loop did. If PPP
// still cycles after this, the next step is doc 182 Fix F(a)
// (ENABLE_AGPS=0 entirely) or F(c) (mutex-serialize ALL AT access
// across GNSS bring-up and the PPP dial) — neither implemented here.
#define ENABLE_AGPS                1

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
// ⚠ COMPLETE PIN RE-MAP (2026-08-03, doc 171) — the PREVIOUS values in
// this block were carried over from the sibling esp32_wave_board_test
// project (a bare ESP32-S3 devkit) and were WRONG for THIS board. On the
// real Waveshare ESP32-S3-A7670E-4G, EVERY ONE of the old pins collided
// with a peripheral that is physically soldered to the board — verified
// against Waveshare's own pinout diagram
// (docs/TestFunctionalities/esp32s3_board/ESP32-S3-A7670E-4G-details-inter.jpg):
//
//   OLD MOSI 42 = camera VSYNC      OLD SCLK 41 = camera HREF
//   OLD CS   39 = camera XCLK       OLD DC   40 = modem RI
//   OLD RST  45 = modem DTR         OLD BL    6 = TF-card DATA
//   OLD SDA  15 = camera SIO_DAT    OLD SCL   7 = camera Y2
//   OLD TP_RST 8 = camera Y3        OLD TP_INT 14 = camera Y9
//   OLD SD_CS 38 = NOT BROKEN OUT AT ALL — no such header pin exists
//
// THE BOARD ONLY EXPOSES FIVE UNCOMMITTED GPIOs: 0, 1, 2, 3, 21.
// Everything else on the two headers is permanently wired to the camera
// FPC connector (7,8,9,10,11,12,13,14,15,16,39,41,42,46), the A7670E
// modem (17,18 = AT-UART1; 19,20 = native USB; 40 = RI; 45 = DTR), the
// TF-card slot (4,5,6), or the UART0 console (43,44).
//
// Five free pins cannot drive a display + touch panel, so ONE peripheral
// had to be given up. The TF-card slot was chosen because this firmware
// never touches it (all storage is SPIFFS on the 16MB internal flash —
// rest_api_storage.c/reference_data.c), whereas the camera is wanted
// later. That releases GPIO 4/5/6 and brings the total to eight.
//
// GPIO 0 is still deliberately avoided (BOOT strapping pin) and four
// display signals need no GPIO at all — they are tied straight to 3V3
// in the harness, which drops the requirement to exactly SEVEN:
//
//   SD_CS  -> 3V3  (only ever had to sit HIGH to keep the unused TF slot
//                   off the shared SPI bus — a fixed pull-up does that
//                   just as well as a GPIO driven high)
//   LCD_RST-> 3V3  (esp_lcd sends the ST7796S software-reset command
//                   when reset_gpio_num is -1 — no hardware pin needed)
//   TP_RST -> 3V3  (FT6336U comes out of power-on reset by itself)
//   TP_INT -> NC   (touch_driver.c polls over I2C; it never read this pin
//                   even when it WAS wired — it was pure dead weight)
//
// Net result: the camera FPC and every modem line stay completely
// untouched, so adding a camera later needs no rewiring at all.
// See doc 171 for the full pin-by-pin table and the wiring diagram.
// ================================================================
// ⚠ REVISION 2 (doc 173/174) — the pin map PROVEN WORKING on real
// hardware in esp32s3_display_taxi_4 on 2026-08-03. Copied here verbatim
// so both projects drive the same harness; no rewiring is needed to move
// between them.
//
// Revision 1 (MOSI 2 / SCLK 21 / CS 1 / DC 3 / RST 0, MISO and SD_CS not
// wired, 40MHz on SPI3) produced a black panel in BOTH projects while
// backlight and touch worked — see doc 174 for the full comparison and
// the ranked root-cause analysis. The two changes most likely responsible
// for the fix are captured below.
//
// SPI2_HOST + IO_MUX: the four bus signals sit on SPI2's DEDICATED
// IO_MUX pins for this chip — FSPID=11 (MOSI), FSPIQ=13 (MISO),
// FSPICLK=12 (SCLK), FSPICS0=10 (CS). ESP-IDF routes these straight
// through the IO_MUX rather than the GPIO matrix, removing the
// propagation delay that makes fast SPI clocks marginal. Espressif rate
// the GPIO matrix at 40MHz max vs 80MHz for IO_MUX — Rev 1 ran at
// exactly 40MHz through the matrix, i.e. at the limit with no margin.
// SD_CS and DC are not SPI-peripheral signals, so they take plain GPIOs
// (9 and 14) inside the same bundle.
//
// COST: the camera. GPIO 9-14 are six of its data lines. Doc 174 §4 sets
// out how some of them might be reclaimed once this is stable — do not
// attempt that until this configuration is confirmed working here too.
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_MISO        13   // Pin 4  — WIRED (was -1) · IO_MUX FSPIQ
#define LCD_MOSI        11   // Pin 5  · IO_MUX FSPID
#define LCD_SCLK        12   // Pin 6  · IO_MUX FSPICLK
#define LCD_SD_CS       9    // Pin 7  — TF slot CS, display_driver.c drives it HIGH (doc 39 Bug #1)
#define LCD_CS          10   // Pin 8  · IO_MUX FSPICS0
#define LCD_DC          14   // Pin 9  — Data/Command
#define LCD_RST         21   // Pin 10 — hardware reset
#define LCD_BL          5    // Pin 11 — unchanged, was already proven working

// 40MHz -> 20MHz: see the IO_MUX note above. Still far faster than this
// UI needs. Raise it again only after the panel is confirmed stable.
#define LCD_PIXEL_CLOCK_HZ  (20 * 1000 * 1000)
#define LCD_BK_LIGHT_ON     1
#define LCD_BK_LIGHT_OFF    0

// ── FT6336U Touch (I2C) ────────────────────────────────────────
// SDA/SCL land on old TF-card pins, which is actually an advantage here:
// SD lines carry board pull-ups, and I2C wants pull-ups anyway. Both were
// already proven working — unchanged in revision 2.
#define TOUCH_I2C_PORT    I2C_NUM_0
#define TOUCH_I2C_SDA     6
#define TOUCH_I2C_SCL     4
#define TOUCH_I2C_ADDR    0x38   // FT6336U 7-bit I2C address
#define TOUCH_INT         2   // Pin 14 — WIRED (was -1); the driver still polls, it never reads this
#define TOUCH_RST         1   // Pin 15 — WIRED (was -1); real reset pulse again

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
//
// ⚠ PHASE 1+2 CORRECTION #2 (2026-07-30, doc 161 §9): 3 -> 2. Real
// hardware showed "Largest DMA block: 4 KB" (via "mem") at the SAME
// moment a real Trips API HTTPS call failed with "esp-aes: Failed to
// allocate memory" — mbedTLS's hardware AES accelerator needs a
// DMA-capable buffer, which (like these draw buffers) can ONLY come
// from internal SRAM, never PSRAM. Doc 157 §5 already listed this exact
// step ("draw buffers 3% -> 2%") as a pre-approved fallback lever if
// more DMA-capable headroom was ever needed — this is that day.
//   At 2%: 320 x (480*2/100 = 9.6 -> 9 rows) x 2 bytes = 5,760 B/buffer,
//          x2 buffers = ~11.25KB internal SRAM.   → frees ~6.25KB more
// Combined with the LVGL pool correction (112->96KB) in the same pass,
// ~22KB total is returned to the general/DMA heap. Cost: slightly more
// visible redraw lag than at 3% — if the UI feels noticeably sluggish,
// this is the first place to raise back (to 3, not all the way to 5).
#define LVGL_BUF_SIZE_PCT  2
#define LVGL_TICK_PERIOD_MS 5

// ================================================================
// PSRAM TASK STACKS — Phase 0 round 3 (doc 155/157/160): move a task's
// STACK (not just its heap allocations) to PSRAM, freeing its internal-
// SRAM footprint entirely.
//
// ⚠ ONLY SAFE FOR A CAREFULLY CHOSEN SUBSET OF TASKS. PSRAM shares the
// same bus/cache mechanism as flash on this chip — any task whose stack
// lives in PSRAM will CRASH if its own call graph ever reaches an NVS/
// SPIFFS/flash write, because the flash-op critical section disables
// that same cache, making the task's OWN stack briefly unreachable to
// itself mid-function. This project's own tasks were individually
// audited (see docs/TestFunctionalities/esp32s3_board/internet--
// hotsport-sms/160_..._ram_buffer_size_list.md "PSRAM-stack audit"
// table) — ONLY lv_tick_task (pure lv_tick_inc(), no I/O at all) and
// ram_test's periodic PSRAM health-check task (heap_caps_malloc/free +
// memset only) were found to NEVER touch flash in their call graph.
// Every other task (bg_worker, serial console, GNSS/NEO-6M readers,
// trip_manager's tick task) either transitively writes NVS/SPIFFS or is
// UART-timing-sensitive — deliberately left on internal-SRAM stacks.
//
// This flag exists so the conversion can be disabled with NO code
// change if it proves unstable on real hardware (untested — no
// toolchain available when this was written) — flip to 0, rebuild.
#define ENABLE_PSRAM_TASK_STACKS   1

// ── UI Defaults ─────────────────────────────────────────────────
#define UI_DEFAULT_FARE_RATE   2.50   // $ per km
#define UI_DEFAULT_FLAG_FALL   3.80   // Base fare

// ================================================================
// NETWORK — cellular internet (PPP over USB CDC) + WiFi hotspot
// (SoftAP + NAPT). See docs/TestFunctionalities/esp32s3_board/
// internet--hotsport-sms/155_..._cellular_ppp_hotspot_sms_analysis_
// and_two_phase_plan.md for the full analysis + hardware verification
// this was built from, and 158_..._phase1_... for the implementation
// record.
//
// ARCHITECTURE (verified on real hardware, doc 155 §12.6): the A7670E
// modem exposes TWO independent channels — PPP data goes over native
// USB CDC (GPIO 19/20, needs DIP USB=OFF), while AT commands (GNSS,
// SMS, signal queries) keep working over UART1 (GPIO 18/17) the whole
// time, completely unaffected by the USB DIP setting or by PPP being
// active. Confirmed empirically: GNSS + AT passthrough + the PC
// console all verified working AT USB=OFF, 2026-07-26.
// ================================================================
#define ENABLE_CELLULAR_PPP   1   // modem internet via USB CDC PPP (iot_usbh_modem) — needs DIP USB=OFF

// doc 169 — the hotspot's on/off state is now a RUNTIME, NVS-persisted
// preference (hotspot_nvs.c), not a build-time-only flag: "hotspot on"/
// "hotspot off" (serial or GUI) persist your choice, and
// hotspot_ap_init() reads it back at every boot so a hotspot you turned
// on stays on across a power cycle instead of reverting to off every
// time (previously it always came up off after a restart no matter what
// you'd set last, since only this compile-time flag was ever checked).
// HOTSPOT_DEFAULT_ENABLED only matters ONCE — a brand-new device's very
// first boot, before any NVS value has ever been written; after that,
// only "hotspot on"/"off" change the persisted state.
#define HOTSPOT_DEFAULT_ENABLED   1

// Which uplink is preferred when BOTH cellular and WiFi-STA are
// available — runtime-switchable via "net uplink wifi|cellular|auto"
// (net_manager.c) without a rebuild; this is only the boot-time default.
#define NET_UPLINK_PREFER_CELLULAR   1

// ── Cellular / USB modem identification ──
// VID:PID + interface indices match the SIMCom A7670E exactly as
// Waveshare's own firmware and the proven esp32s3_4g_hotspotWorkingClaude
// reference project (docs 14/15) both use — not guessed.
#define MODEM_USB_VID          0x1E0E   // SIMCom vendor ID
#define MODEM_USB_PID          0x9011   // A7670E product ID
#define MODEM_USB_CDC_ITF      5        // CDC data interface index
#define MODEM_USB_NOTIF_ITF    (-1)     // no notification interface on this modem

// ── APN profiles — pick the one matching whatever SIM is currently
// inserted (you travel between countries with this board — 2026-07-28:
// Sri Lanka/Hutch; later back to Australia/Vodafone). Change ONLY
// ACTIVE_APN_PROFILE below when you swap SIMs; CELLULAR_APN then follows
// automatically. Add a new PROFILE_* + #elif branch for any other
// carrier instead of overwriting an existing one, so switching back
// later is a one-line change again, not re-typing the APN string.
//
// Hutch (LK) and Dialog (LK) values are from the carriers' own public
// APN settings (verified via web search 2026-07-28 — apn.how, dialog.lk/
// roaming/apn-settings — NOT hand-typed from memory). Vodafone AU is the
// value doc 155 §12/157 already confirmed against the SIM previously in
// this board. Dialog LK has DIFFERENT APNs for prepaid vs postpaid —
// "dialogbb" (postpaid) is set below; switch to "PPWAP" if your Dialog
// line is prepaid and dialing fails.
//
// You don't have to rebuild/reflash to try a different APN — "cell apn
// <apn>" (serial command, cellular_ppp.c) overrides it for the current
// boot only, useful for testing an APN before committing it here.
#define APN_PROFILE_VODAFONE_AU   0
#define APN_PROFILE_HUTCH_LK      1
#define APN_PROFILE_DIALOG_LK     2

// #define ACTIVE_APN_PROFILE   APN_PROFILE_DIALOG_LK   // <-- 2026-07-31: Dialog SIM inserted, Sri Lanka
#define ACTIVE_APN_PROFILE   APN_PROFILE_HUTCH_LK   // <-- 2026-07-31: Dialog SIM inserted, Sri Lanka


#if ACTIVE_APN_PROFILE == APN_PROFILE_VODAFONE_AU
    #define CELLULAR_APN   "live.vodafone.com"       // Vodafone Australia — doc 155 §12/157, confirmed on real SIM
#elif ACTIVE_APN_PROFILE == APN_PROFILE_HUTCH_LK
    #define CELLULAR_APN   "default"                  // Hutch (Sri Lanka) — MCC 413 / MNC 08; carrier's own published APN is literally "default"
#elif ACTIVE_APN_PROFILE == APN_PROFILE_DIALOG_LK
    #define CELLULAR_APN   "dialogbb"                 // Dialog Axiata (Sri Lanka), POSTPAID — MCC 413 / MNC 02; prepaid lines use "PPWAP" instead
#else
    #error "Unknown ACTIVE_APN_PROFILE — add a PROFILE_* + #elif branch above for this carrier"
#endif

// CELLULAR_APN_OVERRIDE="" means "use the profile-selected CELLULAR_APN
// above" — set a non-empty value here to hard-force a specific APN
// regardless of ACTIVE_APN_PROFILE (rare; prefer switching the profile
// instead so this stays the empty/default case).
#define CELLULAR_APN_OVERRIDE   ""

#define CELLULAR_HTTP_TIMEOUT_MS   15000   // unused directly (PPP is raw IP, not HTTP) — reserved for any future modem-side HTTP diagnostics

// ── WiFi Hotspot (SoftAP) defaults ──
// These are FACTORY DEFAULTS ONLY — the live values live in NVS
// (backend/network/hotspot/hotspot_nvs.c) and survive reboot/OTA;
// changing these constants only affects a brand-new device's first boot.
// Runtime change: "hotspot ssid <name>" / "hotspot passwd <old> <new>".
#define HOTSPOT_DEFAULT_SSID       "TaxiMeter-200"
#define HOTSPOT_DEFAULT_PASSWORD   "IamATaxiDriver"   // >=8 chars — WPA2 minimum; shorter silently fails AP start
#define HOTSPOT_DEFAULT_CHANNEL    6
#define HOTSPOT_MAX_CLIENTS        8
#define HOTSPOT_AP_IP              "192.168.4.1"
// Pushed to DHCP clients until real carrier DNS arrives (net_manager.c
// then prepends the carrier's own DNS once PPP is up) — Google + Cloudflare,
// two independent public resolvers, matches the reference project's own
// "static fallback + carrier DNS on top" design (doc 15 §9.2/§9.3).
#define HOTSPOT_DNS_PRIMARY        "8.8.8.8"
#define HOTSPOT_DNS_SECONDARY      "1.1.1.1"

// ================================================================
// SMS — Phase 2 (doc 155/159). Receive+log every SMS, send SMS, and
// execute a small set of remote commands via a three-layer safety gate
// (a message cannot be treated as a command just because it arrived —
// your explicit requirement). See backend/network/sms/sms_commands.h for the
// full gate design and the exact command list.
//
// NOTE ON THE EXISTING "sms <command text>" SERIAL COMMAND
// (additional_work.c, ENABLE_ADDITIONAL_WORK): that one is a pre-
// existing, UNRELATED bench-test simulator ("pretend an SMS with this
// body just arrived") — its own header comment already anticipated
// this exact phase ("wire its message received callback to call
// sms_command_execute() directly... nothing about THIS function needs
// to change"). Left completely untouched — the REAL SMS runtime added
// here uses the "smsc" prefix (SMS Client) for its own serial commands
// specifically to avoid colliding with "sms" in the dispatch chain
// (first-prefix-match-wins — same class of collision net_diag.c/
// net_manager.c already had for "net", resolved the same way: give the
// newer module a different prefix rather than touch a proven file).
// ================================================================
#define ENABLE_SMS   1

// ── Layer 1: sender whitelist ───────────────────────────────────
// EMPTY (count 0) means "reject every command, from anyone" — the safe
// default until you explicitly add your own phone number(s) below.
// Format: exactly how the modem reports the sender (usually
// "+<countrycode><number>", e.g. "+61412345678" — confirm the real
// format against a live "smsc list" entry before relying on this).
// SMS_COMMAND_SENDER_COUNT MUST match the real non-empty entry count —
// it exists as a separate constant (not sizeof/strlen-derived) so the
// gate's "reject all" default is a single obvious number to flip, not
// something that silently changes if the array literal is edited.
#define SMS_COMMAND_SENDER_WHITELIST   { "" }
#define SMS_COMMAND_SENDER_COUNT       0

// ── Layer 2: command prefix ─────────────────────────────────────
// A received SMS body must start with this (case-sensitive) to even be
// CONSIDERED a command attempt — anything else is just logged as a
// normal received SMS (inbox), never executed, regardless of sender.
#define SMS_COMMAND_PREFIX             "TAXI#"

// ── Layer 3: passcode ────────────────────────────────────────────
// Required ONLY for the destructive REBOOT command (see below) — the
// 3 read-only status commands (STATUS/LOCATE/NET) need whitelist+prefix
// only, no passcode, since they can't change or destroy anything.
// Deliberately a SEPARATE constant from FACTORY_RESET_PASSCODE/hotspot's
// reset passcode — rotating one must not silently rotate the others.
#define SMS_COMMAND_PASSCODE           "1010"

// "TAXI#REBOOT <passcode>" — trip-safety interlock (your requirement:
// "do not lose data mid-trip... show a confirm dialog but force reboot
// after a timeout so the command can't be indefinitely evaded"). On
// acceptance: a bounded trip-sync flush is attempted (if a trip is
// active), then a confirm dialog appears on whatever screen is
// currently displayed; tapping "Reboot Now" reboots immediately,
// otherwise this timeout forces it regardless — see sms_commands.c.
#define SMS_REBOOT_CONFIRM_TIMEOUT_S    120
#define SMS_REBOOT_SYNC_FLUSH_TIMEOUT_S  10   // bounded — never lets a stuck sync hold up the whole interlock indefinitely

// Reliability sweep (sms_client.c) — catches any SMS whose +CMTI URC was
// missed (e.g. arrived during boot, before the URC handler was
// registered) by periodically listing unread messages directly, in
// addition to the normal URC-driven path.
#define SMS_SWEEP_INTERVAL_S             60

// In-RAM inbox (not persisted to NVS/SPIFFS — see sms_client.h for why):
// how many of the most recent received SMS the GUI inbox/"smsc list"
// serial command keep available.
#define SMS_INBOX_CAPACITY               10
