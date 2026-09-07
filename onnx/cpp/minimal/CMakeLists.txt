cmake_minimum_required(VERSION 3.16)

project(run_minimal_model LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(
    ONNXRUNTIME_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../onnxruntime-linux-x64-1.29.0"
    CACHE PATH
    "Extracted ONNX Runtime directory"
)

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
    PATHS "${ONNXRUNTIME_ROOT}/lib"
    NO_DEFAULT_PATH
    REQUIRED
)

add_executable(run_minimal_model run_minimal_model.cpp)
target_include_directories(run_minimal_model PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
target_link_libraries(run_minimal_model PRIVATE "${ONNXRUNTIME_LIBRARY}")
target_compile_options(run_minimal_model PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(
    run_minimal_model
    PRIVATE
    MINIMAL_MODEL_PATH="${CMAKE_CURRENT_LIST_DIR}/../../minimal.onnx"
)
set_target_properties(
    run_minimal_model
    PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib"
)
