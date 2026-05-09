# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES
        esp_codec_dev
        esp_http_client
        esp_netif
        esp_wifi
        esp_event
        nvs_flash
        driver
)
