cmake_minimum_required(VERSION 3.16)
idf_component_register(
    SRCS "app_main.c" "version.c" "ota_client.c" "trips_api.c" "mini_command.c" "factory_reset.c" "nvs_state.c" "remote_config.c" "additional_work.c" "ram_test.c" "psram_download_test.c"
    INCLUDE_DIRS "."
    REQUIRES esp_partition spi_flash esp_hw_support esp_system esp_common heap
             esp_psram
             nvs_flash spiffs
             esp_wifi esp_netif esp_event
             esp_http_client esp_https_ota app_update
             json mbedtls
)
