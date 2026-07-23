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
    INCLUDE_DIRS "." "backend" "display" "display/trip" "display/test" "display/test/color_palette" "ui_components"
    REQUIRES esp_partition spi_flash esp_hw_support esp_system esp_common heap
             esp_psram
             nvs_flash spiffs
             esp_wifi esp_netif esp_event
             esp_http_client esp_https_ota app_update
             json mbedtls
             driver lvgl esp_lcd
)
