cmake_minimum_required(VERSION 3.16)

project(gstminimalfilter LANGUAGES CXX)

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

pkg_check_modules(
    OPENCV REQUIRED
    IMPORTED_TARGET
    opencv4
)

add_library(
    gstminimalfilter
    SHARED
    src/minimalfilter.cpp
)

target_link_libraries(
    gstminimalfilter
    PRIVATE
    PkgConfig::GST
)

set_target_properties(
    gstminimalfilter
    PROPERTIES
    OUTPUT_NAME "gstminimalfilter"
)

add_library(
    gstpropertyfilter
    SHARED
    src/propertyfilter.cpp
)

target_link_libraries(
    gstpropertyfilter
    PRIVATE
    PkgConfig::GST
)

set_target_properties(
    gstpropertyfilter
    PROPERTIES
    OUTPUT_NAME "gstpropertyfilter"
)
