cmake_minimum_required(VERSION 3.16.0)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_SOURCE_DIR}/components/esp-webrtc-solution/components/esp_peer)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cardputer-whip)
