cmake_minimum_required(VERSION 3.16)

project(gstminimalfilter LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DEFAULT_ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime-linux-x64-1.29.0")
set(ONNXRUNTIME_ROOT "${DEFAULT_ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime directory")
set(OLD_ONNXRUNTIME_ROOTS
    "${CMAKE_CURRENT_SOURCE_DIR}/demos/onnxruntime-linux-x64-1.29.0"
    "${CMAKE_CURRENT_SOURCE_DIR}/demos/ort_cpu_demo/onnxruntime-linux-x64-1.29.0"
)
if(NOT ONNXRUNTIME_ROOT OR ONNXRUNTIME_ROOT IN_LIST OLD_ONNXRUNTIME_ROOTS)
    set(ONNXRUNTIME_ROOT "${DEFAULT_ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime directory" FORCE)
endif()

unset(ONNXRUNTIME_INCLUDE_DIR CACHE)
unset(ONNXRUNTIME_LIBRARY CACHE)
find_path(
    ONNXRUNTIME_INCLUDE_DIR
    onnxruntime_cxx_api.h
    PATHS "${ONNXRUNTIME_ROOT}/include"
    NO_DEFAULT_PATH
    REQUIRED
)
find_library(
    ONNXRUNTIME_LIBRARY
    onnxruntime
    PATHS "${ONNXRUNTIME_ROOT}/lib" "${ONNXRUNTIME_ROOT}/lib64"
    NO_DEFAULT_PATH
    REQUIRED
)
add_library(ONNXRuntime::ONNXRuntime SHARED IMPORTED)
set_target_properties(
    ONNXRuntime::ONNXRuntime
    PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
)

add_subdirectory(demos)

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
    gstminimalsrc
    SHARED
    src/minimalsrc.cpp
)

target_link_libraries(
    gstminimalsrc
    PRIVATE
    PkgConfig::GST
)

set_target_properties(
    gstminimalsrc
    PROPERTIES
    OUTPUT_NAME "gstminimalsrc"
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

add_library(
    gstmetaattach
    SHARED
    src/metaattach.cpp
)

target_link_libraries(
    gstmetaattach
    PRIVATE
    PkgConfig::GST
)

set_target_properties(
    gstmetaattach
    PROPERTIES
    OUTPUT_NAME "gstmetaattach"
)

add_library(
    gstmetaprint
    SHARED
    src/metaprint.cpp
)

target_link_libraries(
    gstmetaprint
    PRIVATE
    PkgConfig::GST
)

set_target_properties(
    gstmetaprint
    PROPERTIES
    OUTPUT_NAME "gstmetaprint"
)

add_library(
    gstsimplevideosrc
    SHARED
    src/simplevideosrc.cpp
)

target_link_libraries(
    gstsimplevideosrc
    PRIVATE
    PkgConfig::GST
)

set_target_properties(
    gstsimplevideosrc
    PROPERTIES
    OUTPUT_NAME "gstsimplevideosrc"
)

add_library(
    gstislivesrc
    SHARED
    src/is_livesrc.cpp
)

target_link_libraries(
    gstislivesrc
    PRIVATE
    PkgConfig::GST
)

set_target_properties(
    gstislivesrc
    PROPERTIES
    OUTPUT_NAME "gstislivesrc"
)

add_library(
    gstgrayfilter
    SHARED
    src/grayfilter.cpp
)

target_link_libraries(
    gstgrayfilter
    PRIVATE
    PkgConfig::GST
    PkgConfig::OPENCV
)

set_target_properties(
    gstgrayfilter
    PROPERTIES
    OUTPUT_NAME "gstgrayfilter"
)

add_library(
    gstcolorboxsrc
    SHARED
    src/colorboxsrc.cpp
)

target_link_libraries(
    gstcolorboxsrc
    PRIVATE
    PkgConfig::GST
)

set_target_properties(
    gstcolorboxsrc
    PROPERTIES
    OUTPUT_NAME "gstcolorboxsrc"
)

add_library(
    gstreddetect
    SHARED
    src/reddetect.cpp
)

target_link_libraries(
    gstreddetect
    PRIVATE
    PkgConfig::GST
    PkgConfig::OPENCV
)

set_target_properties(
    gstreddetect
    PROPERTIES
    OUTPUT_NAME "gstreddetect"
)

add_library(
    gstcontrolledreddetect
    SHARED
    src/controlledreddetect.cpp
)

target_link_libraries(
    gstcontrolledreddetect
    PRIVATE
    PkgConfig::GST
    PkgConfig::OPENCV
)

set_target_properties(
    gstcontrolledreddetect
    PROPERTIES
    OUTPUT_NAME "gstcontrolledreddetect"
)

add_subdirectory(src/yolodetect)
