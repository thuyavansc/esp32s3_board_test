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

# Dedicated, pre-flashed SPIFFS image for the LLM model + tokenizer — see
# partitions_16mb.csv's `llm` partition (deliberately separate from
# `storage`, doc 125). Regenerated from data/llm/ every time the project is
# flashed (FLASH_IN_PROJECT), same pattern the reference repo itself uses.
spiffs_create_partition_image(llm ../data/llm FLASH_IN_PROJECT)
