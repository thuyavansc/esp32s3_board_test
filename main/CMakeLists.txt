cmake_minimum_required(VERSION 3.16)
idf_component_register(
    SRCS "app_main.c" "version.c"
         "backend/bg_worker.c"
         "backend/system/ota_client.c" "backend/system/mini_command.c"
         "backend/system/factory_reset.c" "backend/system/nvs_state.c" "backend/system/remote_config.c"
         "backend/system/additional_work.c" "backend/system/ram_test.c" "backend/system/psram_download_test.c"
         "backend/system/net_diag.c"
         "backend/taximeter/session_store.c" "backend/taximeter/api_client.c"
         "backend/taximeter/setup_client.c" "backend/taximeter/auth_client.c" "backend/taximeter/duty_client.c"
         "backend/taximeter/reference_data.c" "backend/taximeter/fare_calc.c" "backend/taximeter/trip_manager.c"
         "backend/taximeter/directions_client.c" "backend/taximeter/trip_sync.c"
         "backend/taximeter/diag.c" "backend/taximeter/rest_api_storage.c"
         "backend/gps/gps_client.c" "backend/gps/gps_nmea.c"
         "backend/gps/gps_backend_neo6m.c" "backend/gps/gps_backend_gnss.c"
         "backend/network/hotspot/hotspot_nvs.c" "backend/network/hotspot/hotspot_ap.c"
         "backend/network/cellular/cellular_ppp.c" "backend/network/net_manager.c"
         "backend/network/sms/sms_client.c" "backend/network/sms/sms_commands.c"
         "display/display_driver.c" "display/touch_driver.c" "display/ui_main.c" "display/ui_widgets.c"
         "display/trip/trip_screen.c" "display/trip/trip_json_viewer.c"
         "display/network/network_screen.c"
         "display/sms/sms_screen.c"
         "display/test/test_menu.c" "display/test/test_pax_meter.c"
         "display/test/color_palette/color_palette_ui.c"
         "ui_components/numeric_keypad.c" "ui_components/text_keypad.c" "ui_components/toast.c"
         "ui_components/loading_overlay.c" "ui_components/confirm_dialog.c"
         "llm/llm.c" "llm/llm_runner.c"
    INCLUDE_DIRS "." "backend" "backend/system" "backend/taximeter" "backend/gps"
                 "backend/network" "backend/network/cellular" "backend/network/hotspot" "backend/network/sms"
                 "display" "display/trip" "display/network" "display/sms" "display/test" "display/test/color_palette" "ui_components" "llm"
    REQUIRES esp_partition spi_flash esp_hw_support esp_system esp_common heap
             esp_psram esp_timer
             nvs_flash spiffs
             esp_wifi esp_netif esp_event
             esp_http_client esp_https_ota app_update
             json mbedtls
             driver lvgl esp_lcd
             esp-dsp
             # PHASE 1 (doc 155/158): USB host stack for the cellular
             # modem's native USB CDC path. iot_usbh_modem/iot_usbh_cdc
             # (idf_component.yml) declare their OWN transitive REQUIRES
             # (usb, esp_netif, esp_wifi) automatically via the component
             # manager — "usb" is listed here too anyway for explicit
             # clarity, matching this project's existing style of never
             # relying on an implicit transitive REQUIRES if the real
             # dependency is known.
             usb
)

# llm.c ported from https://github.com/DaveBben/esp32-llm — the author's own
# README notes this exact compiler workaround is needed for a GCC bug when
# building llama2.c-derived code (see esp-idf#11696); scoped to just this one
# file, not the whole project.
set_source_files_properties("llm/llm.c" PROPERTIES COMPILE_OPTIONS "-fno-if-conversion")

# NOTE: spiffs_create_partition_image()/FLASH_IN_PROJECT is the correct call
# for a NATIVE `idf.py flash` build (what the reference repo itself uses) —
# but under PlatformIO's own build wrapper for the espidf framework, this
# custom CMake target is registered (confirmed: it shows up in the CMake
# API reply) but is NEVER actually invoked by `pio run` or `pio run -t
# upload` — no llm.bin ever gets generated, so the model never reaches the
# device this way. Confirmed by testing: `pio run -t buildfs` also doesn't
# use this — PlatformIO has its own separate, incompatible mechanism (see
# doc 125/127) that packages the whole data/ folder into one image and
# auto-picks a single spiffs-type partition, not compatible with this
# project having TWO (storage + llm). The actual, verified-working way to
# get the model onto the `llm` partition under PlatformIO is a manual
# mkspiffs + esptool.py command pair — see doc 125/127 for the exact,
# tested commands. Intentionally NOT calling spiffs_create_partition_image()
# here — it would be dead code under this build system, misleading anyone
# reading this file into thinking the model gets flashed automatically.

# ════════════════════════════════════════════════════════════════
# TaxiMeter backend (backend/taximeter/, backend/gps/) — ported from
# esp32_display_taxi_meter for the real fare-calculation + provisioning/
# auth/duty/reference-data flow. See
# docs/TestFunctionalities/esp32s3_board/
# 135_2026-07-25_fare_calc_and_backend_integration_plan.md for the full
# integration plan and rationale (folder layout, which trip-fetch module
# was kept, GPS backend design).
#
# backend/gps/ — TWO GPS backends running in PARALLEL (not mutually
# exclusive): gps_backend_gnss.c (A7670E built-in GNSS + staged A-GPS,
# over the modem's own AT-command UART1) and gps_backend_neo6m.c
# (external NEO-6M module, UART2) — both share gps_nmea.c's parser
# (both backends emit identical NMEA) and both publish through
# gps_client.c's dispatcher, which decides which one feeds fare_calc.
# See docs/TestFunctionalities/esp32s3_board/
# 139_2026-07-25_gnss_agps_implementation_research_and_plan.md and
# 140_2026-07-25_agps_how_it_actually_works_explained.md.
#
# g2_protocol.c / local_socket.c / socket_client.c (G2 binary protocol,
# local Node.js test socket, iStartek GPRS socket) are deliberately NOT
# compiled in — unused by this feature set, would only add unwanted
# flash size. Not even present in this project (they were esp32_
# display_taxi_meter-only files, never copied here).
# ════════════════════════════════════════════════════════════════
