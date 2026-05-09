idf_component_register(
    SRCS
        ${CMAKE_SOURCE_DIR}/src/app_atomic_compat.c
        ${CMAKE_SOURCE_DIR}/src/app_codec_g711.c
        ${CMAKE_SOURCE_DIR}/src/app_codec_g722.c
        ${CMAKE_SOURCE_DIR}/src/app_audio_adv_codec.c
        ${CMAKE_SOURCE_DIR}/src/app_audio_fake.c
        ${CMAKE_SOURCE_DIR}/src/app_audio_i2s.c
        ${CMAKE_SOURCE_DIR}/src/app_audio_playback.c
        ${CMAKE_SOURCE_DIR}/src/app_https.c
        ${CMAKE_SOURCE_DIR}/src/app_protocol.c
        ${CMAKE_SOURCE_DIR}/src/app_rtsa.c
        ${CMAKE_SOURCE_DIR}/src/app_rtsa_compat.c
        ${CMAKE_SOURCE_DIR}/src/app_session.c
        ${CMAKE_SOURCE_DIR}/src/app_time_sync.c
        ${CMAKE_SOURCE_DIR}/src/app_vendor_g722_decode.c
        ${CMAKE_SOURCE_DIR}/src/app_vendor_g722_encode.c
        ${CMAKE_SOURCE_DIR}/src/app_wifi.c
        ${CMAKE_SOURCE_DIR}/src/main.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/agora_rtsa_sdk/agora_sdk/include
        ${IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/include
    REQUIRES
        esp_codec_dev
        esp_http_client
        esp_netif
        esp_wifi
        esp_event
        nvs_flash
        driver
)

target_link_directories(${COMPONENT_LIB} PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/agora_rtsa_sdk/agora_sdk/lib/xtensa_lx7
)

target_link_libraries(${COMPONENT_LIB} PUBLIC
    agora-rtc-sdk
    aosl
)

target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--allow-multiple-definition"
)
