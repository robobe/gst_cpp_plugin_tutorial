cmake_minimum_required(VERSION 3.16)

project(nanotrack_rknn_rga LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui videoio)
find_package(PkgConfig REQUIRED)
pkg_check_modules(RGA REQUIRED IMPORTED_TARGET librga)

find_path(RKNN_INCLUDE_DIR rknn_api.h REQUIRED)
find_library(RKNN_LIBRARY rknnrt REQUIRED)

add_executable(nanotrack_rknn_rga nanotrack_rknn_rga.cpp)
target_include_directories(nanotrack_rknn_rga PRIVATE "${RKNN_INCLUDE_DIR}" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(
    nanotrack_rknn_rga
    PRIVATE
    ${OpenCV_LIBS}
    PkgConfig::RGA
    "${RKNN_LIBRARY}"
)
set_target_properties(
    nanotrack_rknn_rga
    PROPERTIES
    SKIP_BUILD_RPATH TRUE
)
