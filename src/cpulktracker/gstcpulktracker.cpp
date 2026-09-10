// CPU Lucas-Kanade tracker. It intentionally mirrors the single-ROI contract of cpunanotrack.
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PACKAGE
#define PACKAGE "cpulktracker"
#endif

GST_DEBUG_CATEGORY_STATIC(lktracker_debug);
#define GST_CAT_DEFAULT lktracker_debug

namespace {
constexpr int kMaxCorners = 100;
constexpr int kMinimumFeatures = 8;

cv::Rect2d parse_roi(const std::string& text, int width, int height)
{
    cv::Rect2d roi; char a, b, c; std::istringstream input(text);
    if (!(input >> roi.x >> a >> roi.y >> b >> roi.width >> c >> roi.height) ||
        a != ',' || b != ',' || c != ',' || !(input >> std::ws).eof() ||
        !std::isfinite(roi.x) || !std::isfinite(roi.y) || !std::isfinite(roi.width) || !std::isfinite(roi.height) ||
        roi.x < 0 || roi.y < 0 || roi.width < 1 || roi.height < 1 ||
        roi.x + roi.width > width || roi.y + roi.height > height)
        throw std::runtime_error("roi must be finite x,y,width,height inside the frame, with dimensions >= 1");
    return roi;
}

float median(std::vector<float> values)
{
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

struct Result { cv::Rect2d box; bool initialized; bool lost; unsigned feature_count; double confidence; };

struct Tracker {
    cv::Mat previous_gray;
    std::vector<cv::Point2f> points;
    cv::Rect2d box;
    unsigned initial_feature_count = 0;
    bool lost = false;

    Result initialize(const cv::Mat& gray, const cv::Rect2d& roi)
    {
        cv::Mat mask = cv::Mat::zeros(gray.size(), CV_8UC1);
        mask(cv::Rect(int(roi.x), int(roi.y), int(roi.width), int(roi.height))) = 255;
        cv::goodFeaturesToTrack(gray, points, kMaxCorners, 0.01, 7.0, mask);
        previous_gray = gray.clone(); box = roi;
        initial_feature_count = static_cast<unsigned>(points.size());
        lost = initial_feature_count < kMinimumFeatures;
        return {box, true, lost, initial_feature_count, 0.0};
    }

    Result track(const cv::Mat& gray)
    {
        if (lost) return {box, false, true, 0, 0.0};
        std::vector<cv::Point2f> next, backward, kept;
        std::vector<uchar> forward_status, backward_status;
        std::vector<float> forward_error, backward_error;
        cv::calcOpticalFlowPyrLK(previous_gray, gray, points, next, forward_status, forward_error, cv::Size(21, 21), 3);
        cv::calcOpticalFlowPyrLK(gray, previous_gray, next, backward, backward_status, backward_error, cv::Size(21, 21), 3);
        std::vector<float> dx, dy;
        for (size_t index = 0; index < points.size(); ++index) {
            if (!forward_status[index] || !backward_status[index] || cv::norm(points[index] - backward[index]) > 1.0) continue;
            kept.push_back(next[index]); dx.push_back(next[index].x - points[index].x); dy.push_back(next[index].y - points[index].y);
        }
        previous_gray = gray.clone();
        points = std::move(kept);
        if (points.size() < kMinimumFeatures) {
            lost = true;
            return {box, false, true, static_cast<unsigned>(points.size()), 0.0};
        }
        box.x += median(std::move(dx)); box.y += median(std::move(dy));
        return {box, false, false, static_cast<unsigned>(points.size()), double(points.size()) / initial_feature_count};
    }
};

struct State {
    // ponytail: one lock keeps live ROI changes and frame state consistent; split only if profiling proves it matters.
    std::mutex mutex;
    std::string roi;
    bool enabled = false;
    bool reset = true;
    GstVideoInfo info{};
    Tracker tracker;
};
} // namespace

typedef struct _GstCpuLkTracker { GstBaseTransform parent; State* state; } GstCpuLkTracker;
typedef struct _GstCpuLkTrackerClass { GstBaseTransformClass parent_class; } GstCpuLkTrackerClass;
G_DEFINE_TYPE(GstCpuLkTracker, gst_cpu_lk_tracker, GST_TYPE_BASE_TRANSFORM)
enum { PROP_0, PROP_ENABLED, PROP_ROI };

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=BGR,width=[10,16384],height=[10,16384],interlace-mode=progressive"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=BGR,width=[10,16384],height=[10,16384],interlace-mode=progressive"));

static void set_property(GObject* object, guint id, const GValue* value, GParamSpec* spec)
{
    auto& state = *reinterpret_cast<GstCpuLkTracker*>(object)->state;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (id == PROP_ENABLED) { if (state.enabled != bool(g_value_get_boolean(value))) state.reset = true; state.enabled = g_value_get_boolean(value); }
    else if (id == PROP_ROI) { const char* text = g_value_get_string(value); state.roi = text ? text : ""; state.reset = true; }
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void get_property(GObject* object, guint id, GValue* value, GParamSpec* spec)
{
    auto& state = *reinterpret_cast<GstCpuLkTracker*>(object)->state;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (id == PROP_ENABLED) g_value_set_boolean(value, state.enabled);
    else if (id == PROP_ROI) g_value_set_string(value, state.roi.c_str());
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static gboolean set_caps(GstBaseTransform* base, GstCaps* caps, GstCaps*)
{
    auto& state = *reinterpret_cast<GstCpuLkTracker*>(base)->state;
    std::lock_guard<std::mutex> lock(state.mutex); GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGR || GST_VIDEO_INFO_IS_INTERLACED(&info)) return FALSE;
    if (GST_VIDEO_INFO_WIDTH(&state.info) != GST_VIDEO_INFO_WIDTH(&info) || GST_VIDEO_INFO_HEIGHT(&state.info) != GST_VIDEO_INFO_HEIGHT(&info)) state.reset = true;
    state.info = info; return TRUE;
}

static gboolean sink_event(GstBaseTransform* base, GstEvent* event)
{
    if (GST_EVENT_TYPE(event) == GST_EVENT_STREAM_START || GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT || GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
        auto& state = *reinterpret_cast<GstCpuLkTracker*>(base)->state;
        std::lock_guard<std::mutex> lock(state.mutex); state.reset = true;
    }
    return GST_BASE_TRANSFORM_CLASS(gst_cpu_lk_tracker_parent_class)->sink_event(base, event);
}

static GstFlowReturn transform_ip(GstBaseTransform* base, GstBuffer* buffer)
{
    auto& state = *reinterpret_cast<GstCpuLkTracker*>(base)->state; GstVideoFrame mapped{}; bool mapped_ok = false;
    try {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) state.reset = true;
        if (!state.enabled) return GST_FLOW_OK;
        if (!gst_video_frame_map(&mapped, &state.info, buffer, GST_MAP_READ)) throw std::runtime_error("Cannot map BGR frame");
        mapped_ok = true;
        const int width = GST_VIDEO_FRAME_WIDTH(&mapped), height = GST_VIDEO_FRAME_HEIGHT(&mapped), stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0);
        if (stride < width * 3) throw std::runtime_error("Invalid BGR stride");
        cv::Mat image(height, width, CV_8UC3, GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0), stride), gray;
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
        const Result result = state.reset ? state.tracker.initialize(gray, parse_roi(state.roi, width, height)) : state.tracker.track(gray);
        state.reset = false;
        gst_video_frame_unmap(&mapped); mapped_ok = false;
        const int x = std::clamp(int(std::floor(result.box.x)), 0, width), y = std::clamp(int(std::floor(result.box.y)), 0, height);
        const int right = std::clamp(int(std::ceil(result.box.x + result.box.width)), 0, width), bottom = std::clamp(int(std::ceil(result.box.y + result.box.height)), 0, height);
        auto* meta = gst_buffer_add_video_region_of_interest_meta(buffer, "flowtrack", x, y, right - x, bottom - y);
        if (!meta) throw std::runtime_error("Cannot attach ROI metadata");
        auto* params = gst_structure_new("flowtrack", "initialized", G_TYPE_BOOLEAN, result.initialized,
                                         "feature-count", G_TYPE_UINT, result.feature_count, nullptr);
        if (!result.initialized) gst_structure_set(params, "confidence", G_TYPE_DOUBLE, result.confidence, nullptr);
        if (result.lost) gst_structure_set(params, "lost", G_TYPE_BOOLEAN, TRUE, nullptr);
        gst_video_region_of_interest_meta_add_param(meta, params);
        return GST_FLOW_OK;
    } catch (const std::exception& error) {
        if (mapped_ok) gst_video_frame_unmap(&mapped);
        GST_ELEMENT_ERROR(base, STREAM, FAILED, ("Lucas-Kanade frame processing failed"), ("%s", error.what()));
        return GST_FLOW_ERROR;
    }
}

static void finalize(GObject* object) { delete reinterpret_cast<GstCpuLkTracker*>(object)->state; G_OBJECT_CLASS(gst_cpu_lk_tracker_parent_class)->finalize(object); }
static void gst_cpu_lk_tracker_class_init(GstCpuLkTrackerClass* klass)
{
    auto* object = G_OBJECT_CLASS(klass); auto* element = GST_ELEMENT_CLASS(klass); auto* transform = GST_BASE_TRANSFORM_CLASS(klass);
    object->set_property = set_property; object->get_property = get_property; object->finalize = finalize;
    const auto live = GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING);
    g_object_class_install_property(object, PROP_ENABLED, g_param_spec_boolean("enabled", "Enabled", "Track the configured ROI", FALSE, live));
    g_object_class_install_property(object, PROP_ROI, g_param_spec_string("roi", "ROI", "Initial x,y,width,height in frame pixels; assignment resets tracking", "", live));
    gst_element_class_set_static_metadata(element, "CPU Lucas-Kanade Tracker", "Filter/Metadata/Video", "Tracks one ROI with OpenCV optical flow", "example");
    gst_element_class_add_static_pad_template(element, &sink_template); gst_element_class_add_static_pad_template(element, &src_template);
    transform->set_caps = GST_DEBUG_FUNCPTR(set_caps); transform->sink_event = GST_DEBUG_FUNCPTR(sink_event); transform->transform_ip = GST_DEBUG_FUNCPTR(transform_ip);
}
static void gst_cpu_lk_tracker_init(GstCpuLkTracker* self)
{
    self->state = new State; gst_video_info_init(&self->state->info);
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE); gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
}
static gboolean plugin_init(GstPlugin* plugin)
{ GST_DEBUG_CATEGORY_INIT(lktracker_debug, "cpulktracker", 0, "CPU Lucas-Kanade tracker"); return gst_element_register(plugin, "cpulktracker", GST_RANK_NONE, gst_cpu_lk_tracker_get_type()); }
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, cpulktracker, "CPU Lucas-Kanade tracking metadata", plugin_init, "1.0", "LGPL", "cpulktracker", "https://example.com")
