idf_component_register(
    SRCS
        ${CMAKE_SOURCE_DIR}/src/app_atomic_compat.c
        ${CMAKE_SOURCE_DIR}/src/app_codec_g711.c
        ${CMAKE_SOURCE_DIR}/src/app_audio_fake.c
        ${CMAKE_SOURCE_DIR}/src/app_audio_i2s.c
        ${CMAKE_SOURCE_DIR}/src/app_https.c
        ${CMAKE_SOURCE_DIR}/src/app_time_sync.c
        ${CMAKE_SOURCE_DIR}/src/app_webrtc.c
        ${CMAKE_SOURCE_DIR}/src/app_webrtc_probe.c
        ${CMAKE_SOURCE_DIR}/src/app_whip.c
        ${CMAKE_SOURCE_DIR}/src/app_wifi.c
        ${CMAKE_SOURCE_DIR}/src/main.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/src
        ${IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/include
    REQUIRES esp_peer
)
