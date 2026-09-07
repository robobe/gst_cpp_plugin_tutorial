cmake_minimum_required(VERSION 3.18)
project(cpunanotrack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0)
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/onnxruntime-linux-x64-1.29.0"
    CACHE PATH "ONNX Runtime SDK directory")
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h PATHS "${ONNXRUNTIME_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
find_library(ONNXRUNTIME_LIBRARY onnxruntime PATHS "${ONNXRUNTIME_ROOT}/lib" "${ONNXRUNTIME_ROOT}/lib64" NO_DEFAULT_PATH REQUIRED)
add_library(ONNXRuntime::ONNXRuntime SHARED IMPORTED)
set_target_properties(ONNXRuntime::ONNXRuntime PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}")

add_library(gstcpunanotrack SHARED gstcpunanotrack.cpp)
target_include_directories(gstcpunanotrack PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(gstcpunanotrack PRIVATE PkgConfig::GST ${OpenCV_LIBS} ONNXRuntime::ONNXRuntime)
target_compile_options(gstcpunanotrack PRIVATE -Wall -Wextra)
# Keep the native build RPATH so the bundled ONNX Runtime is found by the plugin scanner.

add_library(gstmetaprint SHARED ../metaprint.cpp)
target_link_libraries(gstmetaprint PRIVATE PkgConfig::GST)

add_executable(cpunanotracker-check check.cpp)
target_link_libraries(cpunanotracker-check PRIVATE PkgConfig::GST)
target_compile_options(cpunanotracker-check PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME cpunanotracker COMMAND cpunanotracker-check "${CMAKE_CURRENT_SOURCE_DIR}/../../demos/nanotracker/onnx")
set_tests_properties(cpunanotracker PROPERTIES ENVIRONMENT "GST_PLUGIN_PATH=$<TARGET_FILE_DIR:gstcpunanotrack>" TIMEOUT 120)
