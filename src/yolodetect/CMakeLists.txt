pkg_check_modules(
    GST_ANALYTICS REQUIRED
    IMPORTED_TARGET
    gstreamer-analytics-1.0
)

add_library(gstyolodetect SHARED yolodetect.cpp)

target_link_libraries(
    gstyolodetect
    PRIVATE
    PkgConfig::GST
    PkgConfig::GST_ANALYTICS
    PkgConfig::OPENCV
    ONNXRuntime::ONNXRuntime
)

set_target_properties(
    gstyolodetect
    PROPERTIES
    OUTPUT_NAME "gstyolodetect"
)
