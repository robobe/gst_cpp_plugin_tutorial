cmake_minimum_required(VERSION 3.18)
project(cpulktracker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-base-1.0 gstreamer-video-1.0 gstreamer-app-1.0)
find_package(OpenCV REQUIRED COMPONENTS core imgproc video)

add_library(gstcpulktracker SHARED gstcpulktracker.cpp)
target_link_libraries(gstcpulktracker PRIVATE PkgConfig::GST ${OpenCV_LIBS})
target_compile_options(gstcpulktracker PRIVATE -Wall -Wextra)

add_executable(cpulktracker-check check.cpp)
target_link_libraries(cpulktracker-check PRIVATE PkgConfig::GST)
target_compile_options(cpulktracker-check PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME cpulktracker COMMAND cpulktracker-check)
set_tests_properties(cpulktracker PROPERTIES ENVIRONMENT "GST_PLUGIN_PATH=$<TARGET_FILE_DIR:gstcpulktracker>" TIMEOUT 30)
