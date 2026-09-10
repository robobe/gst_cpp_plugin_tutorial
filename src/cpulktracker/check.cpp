// Synthetic integration check: initialize, translate, then lose all features.
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

static void paint(GstBuffer* buffer, int dx, int dy, bool textured)
{
    GstMapInfo map{}; require(gst_buffer_map(buffer, &map, GST_MAP_WRITE), "Cannot map input frame");
    std::memset(map.data, 0, map.size);
    if (textured) for (int row = 0; row < 4; ++row) for (int column = 0; column < 5; ++column)
        for (int y = 0; y < 5; ++y) for (int x = 0; x < 5; ++x) {
            const int px = 46 + column * 10 + x + dx, py = 36 + row * 8 + y + dy;
            auto* pixel = map.data + (py * 160 + px) * 3; pixel[0] = pixel[1] = pixel[2] = 255;
        }
    gst_buffer_unmap(buffer, &map);
}

static GstVideoRegionOfInterestMeta* flow_meta(GstBuffer* buffer)
{
    gpointer state = nullptr;
    while (auto* raw = gst_buffer_iterate_meta_filtered(buffer, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)) {
        auto* roi = reinterpret_cast<GstVideoRegionOfInterestMeta*>(raw);
        if (roi->roi_type == g_quark_from_static_string("flowtrack")) return roi;
    }
    return nullptr;
}

int main(int argc, char** argv) try {
    gst_init(&argc, &argv); GError* error = nullptr;
    auto* pipeline = gst_parse_launch("appsrc name=in format=time is-live=true ! cpulktracker name=track ! appsink name=out sync=false async=false", &error);
    if (error) throw std::runtime_error(error->message);
    auto* in = gst_bin_get_by_name(GST_BIN(pipeline), "in"); auto* track = gst_bin_get_by_name(GST_BIN(pipeline), "track"); auto* out = gst_bin_get_by_name(GST_BIN(pipeline), "out");
    auto* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR", "width", G_TYPE_INT, 160, "height", G_TYPE_INT, 120, "framerate", GST_TYPE_FRACTION, 30, 1, "interlace-mode", G_TYPE_STRING, "progressive", nullptr);
    g_object_set(in, "caps", caps, nullptr); gst_caps_unref(caps); g_object_set(track, "roi", "40,30,70,50", "enabled", TRUE, nullptr);
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Cannot start pipeline");
    for (int frame = 0; frame < 3; ++frame) {
        auto* input = gst_buffer_new_allocate(nullptr, 160 * 120 * 3, nullptr); paint(input, 4 * std::min(frame, 1), 3 * std::min(frame, 1), frame < 2);
        GstFlowReturn flow; g_signal_emit_by_name(in, "push-buffer", input, &flow); gst_buffer_unref(input); require(flow == GST_FLOW_OK, "Cannot push frame");
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(out), 5 * GST_SECOND); require(sample, "No output sample");
        auto* roi = flow_meta(gst_sample_get_buffer(sample)); require(roi && roi->w && roi->h, "Missing flowtrack ROI metadata");
        auto* params = gst_video_region_of_interest_meta_get_param(roi, "flowtrack"); gboolean initialized = FALSE, lost = FALSE; double confidence = -1;
        require(params && gst_structure_get_boolean(params, "initialized", &initialized), "Missing initialized flag");
        if (frame == 0) require(initialized, "First frame must initialize");
        if (frame == 1) {
            require(!initialized && gst_structure_get_double(params, "confidence", &confidence) && confidence > 0, "Translated frame must have confidence");
            require(std::abs(int(roi->x) - 44) <= 1 && std::abs(int(roi->y) - 33) <= 1, "Flow did not move the ROI");
        }
        if (frame == 2) require(gst_structure_get_boolean(params, "lost", &lost) && lost && gst_structure_get_double(params, "confidence", &confidence) && confidence == 0.0, "Featureless frame must be lost");
        gst_sample_unref(sample);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL); gst_object_unref(in); gst_object_unref(track); gst_object_unref(out); gst_object_unref(pipeline);
    std::cout << "PASS: Lucas-Kanade metadata\n"; return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
