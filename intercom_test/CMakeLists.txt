cmake_minimum_required(VERSION 3.18)
project(intercom_test C)

set(CMAKE_C_STANDARD 11)

# libre location
set(LIBRE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../re-4.6.0")
set(LIBRE_BUILD_DIR "${LIBRE_DIR}/build")

add_executable(intercom_test
    src/main.c
    src/http_server.c
    src/session.c
    src/camera_win.c
    src/audio_out_win.c
    src/audio_in_win.c
    src/rtsp_server.c
    src/rtsp_auth.c
    src/stun_client.c
    src/relay_client.c
)

target_include_directories(intercom_test PRIVATE
    ${LIBRE_DIR}/include
    src
)

# Link libre static library
target_link_libraries(intercom_test PRIVATE
    ${LIBRE_BUILD_DIR}/libre.a
)

# OpenH264 (H.264 video encoder)
find_path(OPENH264_INCLUDE_DIR wels/codec_api.h)
find_library(OPENH264_LIBRARY NAMES openh264)
if(NOT OPENH264_INCLUDE_DIR OR NOT OPENH264_LIBRARY)
    message(FATAL_ERROR "OpenH264 not found")
endif()
target_include_directories(intercom_test PRIVATE ${OPENH264_INCLUDE_DIR})
target_link_libraries(intercom_test PRIVATE ${OPENH264_LIBRARY})

# OpenSSL
find_package(OpenSSL REQUIRED)
target_link_libraries(intercom_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# zlib
find_package(ZLIB REQUIRED)
target_link_libraries(intercom_test PRIVATE ZLIB::ZLIB)

# Threads
find_package(Threads REQUIRED)
target_link_libraries(intercom_test PRIVATE Threads::Threads)

# Windows libs
if(WIN32)
    target_link_libraries(intercom_test PRIVATE
        ws2_32 wsock32 iphlpapi qwave dbghelp winmm stdc++
    )
else()
    target_link_libraries(intercom_test PRIVATE m stdc++)
endif()

# Relay Server (standalone, no libre dependency)
add_executable(relay_server
    relay_server/relay_server.c
)
if(WIN32)
    target_link_libraries(relay_server PRIVATE ws2_32)
endif()
