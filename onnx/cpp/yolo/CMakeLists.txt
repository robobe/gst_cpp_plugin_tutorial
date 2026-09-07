cmake_minimum_required(VERSION 3.16)

project(yolo_inference LANGUAGES CXX)

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
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc)

add_executable(yolo_inference yolo_inference.cpp)
target_include_directories(
    yolo_inference
    PRIVATE
    "${ONNXRUNTIME_INCLUDE_DIR}"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(yolo_inference PRIVATE "${ONNXRUNTIME_LIBRARY}" ${OpenCV_LIBS})
target_compile_options(yolo_inference PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(
    yolo_inference
    PRIVATE
    YOLO_MODEL_PATH="${CMAKE_CURRENT_LIST_DIR}/../../../models/yolov8n.onnx"
    YOLO_IMAGE_PATH="${CMAKE_CURRENT_LIST_DIR}/../../../assets/bus.jpg"
)
set_target_properties(
    yolo_inference
    PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_ROOT}/lib"
)
