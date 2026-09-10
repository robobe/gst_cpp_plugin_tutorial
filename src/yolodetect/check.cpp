// Real-model contract check: every YOLO result is tracker-style ROI metadata.
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char** argv) try
{
    gst_init(&argc, &argv);
    require(argc == 3, "Usage: yolodetect-check MODEL IMAGE");
    GError* error = nullptr;
    const std::string pipeline_text = "filesrc location=\"" + std::string(argv[2]) + "\" ! jpegdec ! videoconvert ! "
        "video/x-raw,format=RGB ! yolodetect model-path=\"" + argv[1] + "\" ! appsink name=output sync=false async=false";
    GstElement* pipeline = gst_parse_launch(pipeline_text.c_str(), &error);
    if (error) {
        const std::string message = error->message;
        g_error_free(error);
        throw std::runtime_error(message);
    }
    GstElement* output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
    require(gst_element_set_state(pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE, "Cannot start pipeline");
    GstSample* sample = gst_app_sink_pull_preroll(GST_APP_SINK(output));
    require(sample != nullptr, "No decoded sample");
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    int count = 0;
    gpointer state = nullptr;
    while (GstMeta* raw = gst_buffer_iterate_meta_filtered(buffer, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)) {
        auto* roi = reinterpret_cast<GstVideoRegionOfInterestMeta*>(raw);
        if (roi->roi_type != g_quark_from_static_string("yolo-detection")) continue;
        require(roi->id == count, "YOLO ROI IDs must be sequential");
        GstStructure* parameters = gst_video_region_of_interest_meta_get_param(roi, "yolo");
        gint class_id = -1;
        gdouble confidence = -1;
        require(parameters && gst_structure_get_int(parameters, "class-id", &class_id) &&
                    gst_structure_get_double(parameters, "confidence", &confidence),
                "Missing YOLO ROI parameters");
        require(class_id >= 0 && std::isfinite(confidence) && confidence >= 0 && confidence <= 1 && roi->w > 0 && roi->h > 0,
                "Invalid YOLO ROI values");
        ++count;
    }
    gst_sample_unref(sample);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(output);
    gst_object_unref(pipeline);
    require(count > 0, "Expected YOLO ROI metadata for bus.jpg");
    std::cout << "PASS: YOLO ROI metadata\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
