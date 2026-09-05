find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc)

set(DEFAULT_ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime-linux-x64-1.29.0")
get_filename_component(
    OLD_ONNXRUNTIME_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../onnxruntime-linux-x64-1.29.0"
    ABSOLUTE
)
set(ONNXRUNTIME_ROOT "${DEFAULT_ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime directory")
if(NOT ONNXRUNTIME_ROOT OR "${ONNXRUNTIME_ROOT}" STREQUAL "${OLD_ONNXRUNTIME_ROOT}")
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

add_executable(ort ort.cpp)
target_include_directories(ort PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
target_link_libraries(ort PRIVATE "${ONNXRUNTIME_LIBRARY}" ${OpenCV_LIBS})

get_filename_component(ONNXRUNTIME_LIBRARY_DIR "${ONNXRUNTIME_LIBRARY}" DIRECTORY)
set_target_properties(ort PROPERTIES BUILD_RPATH "${ONNXRUNTIME_LIBRARY_DIR}")
