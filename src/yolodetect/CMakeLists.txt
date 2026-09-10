pkg_check_modules(GST_APP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)

add_library(gstyolodetect SHARED yolodetect.cpp)

target_link_libraries(
    gstyolodetect
    PRIVATE
    PkgConfig::GST
    PkgConfig::OPENCV
    ONNXRuntime::ONNXRuntime
)

set_target_properties(
    gstyolodetect
    PROPERTIES
    OUTPUT_NAME "gstyolodetect"
)

if(BUILD_TESTING)
    add_executable(yolodetect-check check.cpp)
    target_link_libraries(yolodetect-check PRIVATE PkgConfig::GST PkgConfig::GST_APP)
    target_compile_options(yolodetect-check PRIVATE -Wall -Wextra)
    add_test(
        NAME yolodetect
        COMMAND yolodetect-check
            "${CMAKE_CURRENT_SOURCE_DIR}/../../demos/ort_cpu_demo/yolov8n.onnx"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/bus.jpg"
    )
    set_tests_properties(yolodetect PROPERTIES ENVIRONMENT "GST_PLUGIN_PATH=$<TARGET_FILE_DIR:gstyolodetect>" TIMEOUT 60)
    add_test(
        NAME yolodetect-yolo26
        COMMAND yolodetect-check
            "${CMAKE_CURRENT_SOURCE_DIR}/../../demos/yolo26/yolo26n.onnx"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/bus.jpg"
    )
    set_tests_properties(yolodetect-yolo26 PROPERTIES ENVIRONMENT "GST_PLUGIN_PATH=$<TARGET_FILE_DIR:gstyolodetect>" TIMEOUT 60)
endif()
