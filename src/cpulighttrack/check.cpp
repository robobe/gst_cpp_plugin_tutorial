// Small real-model check: initialization and subsequent tracking metadata must be valid.
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main(int argc, char** argv) try {
    gst_init(&argc, &argv); require(argc == 2, "Usage: cpulighttrack-check MODELS_DIR");
    GError* error = nullptr;
    auto* pipeline = gst_parse_launch("appsrc name=in format=time is-live=true ! cpulighttrack name=track ! appsink name=out sync=false async=false", &error);
    if (error) throw std::runtime_error(error->message);
    auto* in = gst_bin_get_by_name(GST_BIN(pipeline), "in"); auto* track = gst_bin_get_by_name(GST_BIN(pipeline), "track"); auto* out = gst_bin_get_by_name(GST_BIN(pipeline), "out");
    auto* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR", "width", G_TYPE_INT, 320, "height", G_TYPE_INT, 240, "framerate", GST_TYPE_FRACTION, 30, 1, "interlace-mode", G_TYPE_STRING, "progressive", nullptr);
    g_object_set(in, "caps", caps, nullptr); gst_caps_unref(caps); g_object_set(track, "models-dir", argv[1], "roi", "80,60,64,64", "enabled", TRUE, nullptr);
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Cannot start pipeline");
    for (int frame = 0; frame < 2; ++frame) {
        auto* buffer = gst_buffer_new_allocate(nullptr, 320 * 240 * 3, nullptr); GstMapInfo map{}; gst_buffer_map(buffer, &map, GST_MAP_WRITE);
        for (size_t i = 0; i < map.size; ++i)
            map.data[i] = static_cast<guint8>((i + frame * 17) % 251);
        gst_buffer_unmap(buffer, &map);
        GstFlowReturn flow; g_signal_emit_by_name(in, "push-buffer", buffer, &flow); gst_buffer_unref(buffer); require(flow == GST_FLOW_OK, "Cannot push frame");
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(out), 15 * GST_SECOND); require(sample, "No output sample");
        auto* buffer_out = gst_sample_get_buffer(sample); GstVideoRegionOfInterestMeta* roi = nullptr; gpointer state = nullptr;
        while (auto* raw = gst_buffer_iterate_meta_filtered(buffer_out, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)) {
            auto* candidate = reinterpret_cast<GstVideoRegionOfInterestMeta*>(raw);
            if (candidate->roi_type == g_quark_from_static_string("lighttrack")) { roi = candidate; break; }
        }
        require(roi && roi->w && roi->h, "Missing LightTrack ROI metadata");
        auto* parameters = gst_video_region_of_interest_meta_get_param(roi, "lighttrack"); gboolean initialized = FALSE; double confidence = -1; require(parameters && gst_structure_get_boolean(parameters, "initialized", &initialized), "Missing initialized flag");
        require(bool(initialized) == (frame == 0), "Unexpected initialization state");
        if (frame) require(gst_structure_get_double(parameters, "confidence", &confidence) && std::isfinite(confidence) && confidence >= 0 && confidence <= 1, "Invalid confidence");
        gst_sample_unref(sample);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(in); gst_object_unref(track); gst_object_unref(out); gst_object_unref(pipeline);
    std::cout << "PASS: LightTrack ONNX metadata\n"; return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
