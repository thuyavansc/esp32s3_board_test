cmake_minimum_required(VERSION 3.16)
idf_component_register(
    SRCS "app_main.c" "version.c"
         "backend/ota_client.c" "backend/trips_api.c" "backend/mini_command.c"
         "backend/factory_reset.c" "backend/nvs_state.c" "backend/remote_config.c"
         "backend/additional_work.c" "backend/ram_test.c" "backend/psram_download_test.c"
         "backend/bg_worker.c"
         "display/display_driver.c" "display/touch_driver.c" "display/ui_main.c" "display/ui_widgets.c"
         "display/trip/trip_screen.c" "display/trip/trip_json_viewer.c"
         "display/test/test_menu.c" "display/test/test_pax_meter.c"
         "display/test/color_palette/color_palette_ui.c"
         "ui_components/numeric_keypad.c" "ui_components/toast.c"
         "ui_components/loading_overlay.c" "ui_components/confirm_dialog.c"
         "llm/llm.c" "llm/llm_runner.c"
    INCLUDE_DIRS "." "backend" "display" "display/trip" "display/test" "display/test/color_palette" "ui_components" "llm"
    REQUIRES esp_partition spi_flash esp_hw_support esp_system esp_common heap
             esp_psram
             nvs_flash spiffs
             esp_wifi esp_netif esp_event
             esp_http_client esp_https_ota app_update
             json mbedtls
             driver lvgl esp_lcd
             esp-dsp
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
