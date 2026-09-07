// On-board integration check. Uses real RKNN models, no test framework.
#include <gst/gst.h>
#include <gst/video/video.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Pipeline {
public:
    Pipeline(const char* models, const char* precision, const char* resize)
    {
        GError* error = nullptr;
        pipeline = gst_parse_launch("appsrc name=input format=time is-live=true ! "
            "rknnnanotrack name=tracker ! appsink name=output sync=false async=false", &error);
        if (error) {
            std::string message = error->message;
            g_error_free(error);
            if (pipeline) gst_object_unref(pipeline);
            throw std::runtime_error(message);
        }
        input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
        tracker = gst_bin_get_by_name(GST_BIN(pipeline), "tracker");
        output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
        g_object_set(tracker, "models-dir", models, "precision", precision, "resize", resize, nullptr);
        caps(321, 240); // Exercise non-packed rows: 963 pixels bytes, 964-byte stride.
    }
    ~Pipeline()
    {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(input); gst_object_unref(tracker); gst_object_unref(output);
        gst_object_unref(pipeline);
        if (original) gst_buffer_unref(original);
    }
    void play()
    {
        require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Cannot start pipeline");
    }
    void caps(int w, int h)
    {
        width = w; height = h; stride = (w*3+3)&~3;
        GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR",
            "width", G_TYPE_INT, w, "height", G_TYPE_INT, h,
            "framerate", GST_TYPE_FRACTION, 30, 1, "interlace-mode", G_TYPE_STRING, "progressive", nullptr);
        g_object_set(input, "caps", caps, nullptr);
        gst_caps_unref(caps);
    }
    void push(bool discontinuity = false)
    {
        pixels.resize(size_t(stride)*height);
        for (int y=0; y<height; ++y)
            for (int x=0; x<stride; ++x) pixels[size_t(y)*stride+x] = (x*13+y*7)%256;
        auto* buffer = gst_buffer_new_allocate(nullptr, pixels.size(), nullptr);
        gst_buffer_fill(buffer, 0, pixels.data(), pixels.size());
        pts = sequence++ * GST_SECOND / 30;
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
        if (discontinuity) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        gst_buffer_add_video_region_of_interest_meta(buffer, "upstream-test", 1, 2, 3, 4);
        if (original) gst_buffer_unref(original);
        original = gst_buffer_ref(buffer); // Force the element to make a writable buffer header.
        GstFlowReturn flow;
        g_signal_emit_by_name(input, "push-buffer", buffer, &flow);
        gst_buffer_unref(buffer);
        require(flow == GST_FLOW_OK, "Cannot push test buffer");
    }
    void frame(int expected, bool discontinuity = false)
    {
        push(discontinuity);
        GstSample* sample = nullptr;
        g_signal_emit_by_name(output, "try-pull-sample", GstClockTime(15*GST_SECOND), &sample);
        if (!sample) {
            auto* bus = gst_element_get_bus(pipeline);
            auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
            if (message) {
                GError* error = nullptr; gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                std::cerr << error->message << ": " << (debug ? debug : "") << '\n';
                g_error_free(error); g_free(debug); gst_message_unref(message);
            }
            gst_object_unref(bus);
            throw std::runtime_error("No output sample");
        }
        auto* buffer = gst_sample_get_buffer(sample);
        bool correct = GST_BUFFER_PTS(buffer) == pts && GST_BUFFER_DURATION(buffer) == GST_SECOND/30;
        GstMapInfo map{};
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            correct &= map.size == pixels.size() && std::memcmp(map.data, pixels.data(), pixels.size()) == 0;
            gst_buffer_unmap(buffer, &map);
        } else correct = false;
        int count = 0, upstream = 0;
        gpointer state = nullptr;
        while (auto* raw = gst_buffer_iterate_meta_filtered(buffer, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)) {
            auto* roi = reinterpret_cast<GstVideoRegionOfInterestMeta*>(raw);
            if (roi->roi_type == g_quark_from_static_string("upstream-test")) ++upstream;
            if (roi->roi_type != g_quark_from_static_string("nanotrack")) continue;
            ++count;
            auto* parameters = gst_video_region_of_interest_meta_get_param(roi, "nanotrack");
            gboolean initialized = FALSE;
            double confidence = -1;
            correct &= parameters && gst_structure_get_boolean(parameters, "initialized", &initialized);
            const bool has_score = parameters && gst_structure_get_double(parameters, "confidence", &confidence);
            correct &= bool(initialized) == (expected == 1);
            correct &= has_score == (expected == 0);
            if (has_score) correct &= std::isfinite(confidence) && confidence >= 0 && confidence <= 1;
            correct &= roi->w > 0 && roi->h > 0 && roi->x+roi->w <= unsigned(width) && roi->y+roi->h <= unsigned(height);
            if (expected == 1) {
                gchar* configured = nullptr;
                g_object_get(tracker, "roi", &configured, nullptr);
                unsigned x,y,w,h;
                correct &= std::sscanf(configured, "%u,%u,%u,%u", &x,&y,&w,&h) == 4 &&
                           roi->x == x && roi->y == y && roi->w == w && roi->h == h;
                g_free(configured);
            }
        }
        correct &= upstream == 1 && count == (expected < 0 ? 0 : 1);
        correct &= gst_buffer_get_n_meta(original, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE) == 1;
        gst_sample_unref(sample);
        require(correct, "Pixels, timestamps, or ROI metadata did not match contract");
    }
    void expect_error()
    {
        push();
        auto* bus = gst_element_get_bus(pipeline);
        auto* message = gst_bus_timed_pop_filtered(bus, 15*GST_SECOND, GST_MESSAGE_ERROR);
        require(message != nullptr, "Expected a GStreamer error");
        gst_message_unref(message);
        gst_object_unref(bus);
    }
    void event(GstEvent* event)
    {
        auto* pad = gst_element_get_static_pad(input, "src");
        const bool accepted = gst_pad_push_event(pad, event);
        gst_object_unref(pad);
        require(accepted, "Reset event rejected");
    }
    void segment()
    {
        GstSegment segment;
        gst_segment_init(&segment, GST_FORMAT_TIME);
        event(gst_event_new_segment(&segment));
    }
    GstElement *pipeline, *input, *tracker, *output;
private:
    int width = 0, height = 0, stride = 0;
    guint64 sequence = 0;
    GstClockTime pts = 0;
    std::vector<unsigned char> pixels;
    GstBuffer* original = nullptr;
};

int main(int argc, char** argv) try
{
    gst_init(&argc, &argv);
    require(argc >= 2, "Usage: nanotracker-check MODELS_DIR [precision=mixed] [resize=cpu]");
    const char* precision = argc > 2 ? argv[2] : "mixed";
    const char* resize = argc > 3 ? argv[3] : "cpu";
    {
        Pipeline idle("", precision, resize);
        idle.play(); idle.frame(-1); // No ROI or models needed while disabled.
    }
    {
        Pipeline p(argv[1], precision, resize);
        p.play();
        g_object_set(p.tracker, "roi", "80,60,64,64", "enabled", TRUE, nullptr);
        p.frame(1); p.frame(0);
        g_object_set(p.tracker, "roi", "20,20,48,48", nullptr);
        p.frame(1); p.frame(0);
        g_object_set(p.tracker, "roi", "20,20,48,48", nullptr);
        p.frame(1); // Same ROI assignment must also reset.
        g_object_set(p.tracker, "enabled", FALSE, nullptr); p.frame(-1);
        g_object_set(p.tracker, "enabled", TRUE, nullptr); p.frame(1); p.frame(0);
        p.frame(1, true); p.frame(0);
        p.segment(); p.frame(1);
        p.event(gst_event_new_flush_start()); p.event(gst_event_new_flush_stop(TRUE));
        p.segment(); p.frame(1);
        p.event(gst_event_new_stream_start("check-restart")); p.segment(); p.frame(1);
        p.caps(325, 244); p.frame(1); p.frame(0);
        g_object_set(p.tracker, "models-dir", "/must-not-change", "precision", "invalid", "resize", "invalid", nullptr);
        gchar *dir = nullptr, *mode = nullptr, *resizer = nullptr;
        g_object_get(p.tracker, "models-dir", &dir, "precision", &mode, "resize", &resizer, nullptr);
        const bool unchanged = std::string(dir) == argv[1] && std::string(mode) == precision && std::string(resizer) == resize;
        g_free(dir); g_free(mode); g_free(resizer);
        require(unchanged, "Stopped-only properties changed while PLAYING");
        p.frame(0);
        Pipeline other(argv[1], precision, resize);
        g_object_set(other.tracker, "roi", "1,2,32,32", "enabled", TRUE, nullptr);
        other.play(); other.frame(1); p.frame(0); other.frame(0);
        // A 2x2 template crop requires >16x scaling: exercise RGA's CPU fallback.
        g_object_set(other.tracker, "roi", "80,60,1,1", nullptr);
        other.frame(1); other.frame(0); p.frame(0);
        require(gst_element_set_state(p.pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE, "Cannot stop");
        p.play(); p.frame(1); // Runtime must be recreated after stop.
        p.caps(20, 20); p.expect_error(); // Stored ROI no longer fits.
    }
    for (const char* roi : {"", "nan,0,10,10", "0,0,inf,10", "0,0,-1,10", "-1,0,10,10",
                           "0,0,10,10junk", "0,0,9999,10"}) {
        Pipeline p(argv[1], precision, resize);
        g_object_set(p.tracker, "roi", roi, "enabled", TRUE, nullptr);
        p.play(); p.expect_error();
    }
    {
        Pipeline p("/nonexistent-nanotracker-models", precision, resize);
        g_object_set(p.tracker, "roi", "0,0,32,32", "enabled", TRUE, nullptr);
        p.play(); p.expect_error();
    }
    std::cout << "PASS: passthrough, tracking, controls, resets, independent instances, and errors ("
              << precision << ", " << resize << ")\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
