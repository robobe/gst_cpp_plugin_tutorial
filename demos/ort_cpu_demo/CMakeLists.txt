find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc)

add_executable(ort ort.cpp)
target_link_libraries(ort PRIVATE ONNXRuntime::ONNXRuntime ${OpenCV_LIBS})
