cmake_minimum_required(VERSION 3.16)

project(gstrknnyolodetect LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(
    GST REQUIRED
    IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-base-1.0
    gstreamer-video-1.0
)
pkg_check_modules(RGA REQUIRED IMPORTED_TARGET librga)

find_path(RKNN_INCLUDE_DIR rknn_api.h REQUIRED)
find_library(RKNN_LIBRARY rknnrt REQUIRED)

add_library(gstrknnyolodetect SHARED ../src/rknnyolodetect.cpp)
target_include_directories(gstrknnyolodetect PRIVATE "${RKNN_INCLUDE_DIR}")
target_link_libraries(
    gstrknnyolodetect
    PRIVATE
    PkgConfig::GST
    PkgConfig::RGA
    "${RKNN_LIBRARY}"
)
set_target_properties(
    gstrknnyolodetect
    PROPERTIES
    OUTPUT_NAME "gstrknnyolodetect"
    SKIP_BUILD_RPATH TRUE
)

add_library(gstmetaprint SHARED ../src/metaprint.cpp)
target_link_libraries(gstmetaprint PRIVATE PkgConfig::GST)
set_target_properties(
    gstmetaprint
    PROPERTIES
    OUTPUT_NAME "gstmetaprint"
    SKIP_BUILD_RPATH TRUE
)
