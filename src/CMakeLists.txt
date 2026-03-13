cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(SoulCam VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Build type
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -DSOULCAM_DEBUG")

# ---------------------------------------------------------------------------
# Platform flags (RK3566 = Cortex-A55, ARMv8.2-A)
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    add_compile_options(-march=armv8.2-a)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

# GStreamer core + RTSP server
pkg_check_modules(GST       REQUIRED gstreamer-1.0)
pkg_check_modules(GST_APP   REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0)
pkg_check_modules(GST_RTSP  REQUIRED gstreamer-rtsp-server-1.0)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)

# RKNN runtime  (ships as a prebuilt .so; no pkg-config)
# Search order: -DRKNN_DIR, ../rknn, source-dir itself, /usr/local
set(RKNN_DIR "${CMAKE_SOURCE_DIR}/../rknn" CACHE PATH "Path to RKNN SDK dir (with include/ and lib/)")
set(HAVE_RKNN FALSE)
foreach(_rknn_candidate "${RKNN_DIR}" "${CMAKE_SOURCE_DIR}" "/usr/local")
    if(EXISTS "${_rknn_candidate}/include/rknn_api.h")
        set(RKNN_INCLUDE_DIRS "${_rknn_candidate}/include")
        # Prefer local lib, fallback to system
        if(EXISTS "${_rknn_candidate}/lib/librknnrt.so")
            set(RKNN_LIBRARIES "${_rknn_candidate}/lib/librknnrt.so")
        elseif(EXISTS "/usr/local/lib/librknnrt.so")
            set(RKNN_LIBRARIES "/usr/local/lib/librknnrt.so")
        endif()
        if(DEFINED RKNN_LIBRARIES)
            set(HAVE_RKNN TRUE)
            message(STATUS "RKNN found: headers=${_rknn_candidate}/include  lib=${RKNN_LIBRARIES}")
            break()
        endif()
    endif()
endforeach()
if(NOT HAVE_RKNN)
    message(STATUS "RKNN not found -- AI pipeline will be a stub")
endif()

# Cairo (optional, for debug overlay on RTSP stream)
pkg_check_modules(CAIRO cairo)
if(CAIRO_FOUND)
    set(HAVE_CAIRO TRUE)
    message(STATUS "Cairo found: overlay support enabled")
else()
    set(HAVE_CAIRO FALSE)
    message(STATUS "Cairo not found -- overlay disabled (install libcairo2-dev)")
endif()

# OpenCV (optional, for debug/visualization only)
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    set(HAVE_OPENCV TRUE)
    message(STATUS "OpenCV found: ${OpenCV_VERSION}")
else()
    set(HAVE_OPENCV FALSE)
    message(STATUS "OpenCV not found -- debug visualization disabled")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(SOULCAM_SOURCES
    main.cpp
    store/store_config.cpp
    store/store.cpp
    soullink/json.cpp
    soullink/sync_engine.cpp
    soullink/module.cpp
    pipeline/isp_config.cpp
    pipeline/rtsp_server.cpp
    pipeline/ai_capture.cpp
    pipeline/overlay.cpp
    pipeline/onvif_metadata.cpp
    pipeline/onvif_device.cpp
    pipeline/snapshot.cpp
    pipeline/tuya_ipc.cpp
    ai/detector.cpp
    ai/hand_target_tracker.cpp
    ai/model_pipeline.cpp
    util/logger.cpp
)

add_executable(soulcam ${SOULCAM_SOURCES})

target_include_directories(soulcam PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GST_VIDEO_INCLUDE_DIRS}
    ${GST_RTSP_INCLUDE_DIRS}
    ${MOSQUITTO_INCLUDE_DIRS}
)

target_link_libraries(soulcam PRIVATE
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GST_VIDEO_LIBRARIES}
    ${GST_RTSP_LIBRARIES}
    ${MOSQUITTO_LIBRARIES}
    pthread
    dl
)

if(HAVE_RKNN)
    target_include_directories(soulcam PRIVATE ${RKNN_INCLUDE_DIRS})
    target_link_libraries(soulcam PRIVATE ${RKNN_LIBRARIES})
    target_compile_definitions(soulcam PRIVATE SOULCAM_HAVE_RKNN=1)
endif()

if(HAVE_CAIRO)
    target_include_directories(soulcam PRIVATE ${CAIRO_INCLUDE_DIRS})
    target_link_libraries(soulcam PRIVATE ${CAIRO_LIBRARIES})
    target_compile_definitions(soulcam PRIVATE SOULCAM_HAVE_CAIRO=1)
endif()

if(HAVE_OPENCV)
    target_include_directories(soulcam PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(soulcam PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(soulcam PRIVATE SOULCAM_HAVE_OPENCV=1)
endif()

# RGA plugin path -- needed at runtime so rgaconvert is discoverable
target_compile_definitions(soulcam PRIVATE
    SOULCAM_RGA_PLUGIN_PATH="/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0"
)

# ---------------------------------------------------------------------------
# Tuya IPC SDK (optional, for cloud connectivity)
# ---------------------------------------------------------------------------
set(TUYA_SDK_DIR "${CMAKE_SOURCE_DIR}/../tuya_sdk" CACHE PATH "Path to TuyaOS IPC SDK")
set(HAVE_TUYA FALSE)
if(EXISTS "${TUYA_SDK_DIR}/include/tuya_ipc_api.h")
    set(HAVE_TUYA TRUE)
    target_include_directories(soulcam PRIVATE "${TUYA_SDK_DIR}/include")
    target_link_libraries(soulcam PRIVATE "${TUYA_SDK_DIR}/lib/libtuya_ipc.a")
    target_compile_definitions(soulcam PRIVATE SOULCAM_HAVE_TUYA=1)
    message(STATUS "Tuya IPC SDK found: ${TUYA_SDK_DIR}")
else()
    message(STATUS "Tuya IPC SDK not found at ${TUYA_SDK_DIR} -- stub mode (no cloud)")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS soulcam RUNTIME DESTINATION bin)
