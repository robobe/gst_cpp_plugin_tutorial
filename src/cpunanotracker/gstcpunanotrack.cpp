// CPU NanoTrackV3: GStreamer behavior and tracking math adapted from
// src/nanotracker/gstrknnnanotrack.cpp; ONNX preprocessing matches the Python demo.
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <opencv2/imgproc.hpp>
#include <onnxruntime_cxx_api.h>
#include <functional>
#include <numeric>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PACKAGE
#define PACKAGE "cpunanotrack"
#endif
GST_DEBUG_CATEGORY_STATIC(nanotrack_debug);
#define GST_CAT_DEFAULT nanotrack_debug

namespace {
cv::Rect2d parse_roi(const std::string& text, int width, int height)
{
    cv::Rect2d roi;
    char a, b, c;
    std::istringstream input(text);
    if (!(input >> roi.x >> a >> roi.y >> b >> roi.width >> c >> roi.height) ||
        a != ',' || b != ',' || c != ',' || !(input >> std::ws).eof() ||
        !std::isfinite(roi.x) || !std::isfinite(roi.y) ||
        !std::isfinite(roi.width) || !std::isfinite(roi.height) ||
        roi.x < 0 || roi.y < 0 || roi.width < 1 || roi.height < 1 ||
        roi.x + roi.width > width || roi.y + roi.height > height)
        throw std::runtime_error("roi must be finite x,y,width,height inside the frame, with dimensions >= 1");
    return roi;
}

class Model {
public:
    Model(Ort::Env& env, const Ort::SessionOptions& options, const std::string& path,
          std::vector<std::vector<int64_t>> inputs, std::vector<std::vector<int64_t>> outputs)
        : session(env, path.c_str(), options), input_shapes(std::move(inputs)), output_shapes(std::move(outputs))
    {
        if (session.GetInputCount() != input_shapes.size() || session.GetOutputCount() != output_shapes.size())
            throw std::runtime_error("Unexpected NanoTrack ONNX I/O counts: " + path);
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < input_shapes.size(); ++i) {
            auto info = session.GetInputTypeInfo(i);
            validate(info.GetTensorTypeAndShapeInfo(), input_shapes[i]);
            input_names.emplace_back(session.GetInputNameAllocated(i, allocator).get());
        }
        for (size_t i = 0; i < output_shapes.size(); ++i) {
            auto info = session.GetOutputTypeInfo(i);
            validate(info.GetTensorTypeAndShapeInfo(), output_shapes[i]);
            output_names.emplace_back(session.GetOutputNameAllocated(i, allocator).get());
        }
        for (const auto& name : input_names) input_name_ptrs.push_back(name.c_str());
        for (const auto& name : output_names) output_name_ptrs.push_back(name.c_str());
    }
    void run(std::initializer_list<float*> buffers)
    {
        if (buffers.size() != input_shapes.size()) throw std::runtime_error("Incorrect ONNX input count");
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> tensors;
        size_t i = 0;
        for (float* data : buffers) {
            const auto& shape = input_shapes[i++];
            const size_t count = std::accumulate(shape.begin(), shape.end(), size_t(1), std::multiplies<size_t>());
            tensors.push_back(Ort::Value::CreateTensor<float>(memory, data, count, shape.data(), shape.size()));
        }
        values = session.Run(Ort::RunOptions{nullptr}, input_name_ptrs.data(), tensors.data(), tensors.size(),
                             output_name_ptrs.data(), output_name_ptrs.size());
        for (size_t j = 0; j < values.size(); ++j)
            validate(values[j].GetTensorTypeAndShapeInfo(), output_shapes[j]);
    }
    float* output(size_t i) { return values.at(i).GetTensorMutableData<float>(); }
private:
    template<typename Info>
    static void validate(const Info& info, const std::vector<int64_t>& shape)
    {
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetShape() != shape)
            throw std::runtime_error("Expected fixed NanoTrack FLOAT32 NCHW tensor shape");
    }
    Ort::Session session;
    std::vector<std::vector<int64_t>> input_shapes, output_shapes;
    std::vector<std::string> input_names, output_names;
    std::vector<const char*> input_name_ptrs, output_name_ptrs;
    std::vector<Ort::Value> values;
};

Ort::SessionOptions cpu_options()
{
    Ort::SessionOptions options;
    // No accelerated provider is appended: ONNX Runtime uses its default CPU provider.
    // Avoid three independent sessions each creating a full machine-sized thread pool.
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    return options;
}

float padded_size(float w, float h)
{
    const float pad = (w + h) * 0.5f;
    return std::sqrt((w + pad) * (h + pad));
}

struct Result {
    cv::Rect2d box;
    double confidence = 0;
    bool initialized = false;
};

class Tracker {
public:
    explicit Tracker(const std::string& dir)
        : env(ORT_LOGGING_LEVEL_WARNING, "cpunanotrack"), options(cpu_options()),
          template_net(env, options, dir + "/nanotrack_backbone_template.onnx", {{1,3,127,127}}, {{1,96,8,8}}),
          search_net(env, options, dir + "/nanotrack_backbone.onnx", {{1,3,255,255}}, {{1,96,16,16}}),
          head(env, options, dir + "/nanotrack_head.onnx", {{1,96,8,8}, {1,96,16,16}}, {{1,2,15,15}, {1,4,15,15}})
    {
        for (int y = 0, i = 0; y < 15; ++y) {
            for (int x = 0; x < 15; ++x, ++i) {
                window[i] = (0.5f - 0.5f * std::cos(2.f * float(CV_PI) * x / 14)) *
                            (0.5f - 0.5f * std::cos(2.f * float(CV_PI) * y / 14));
            }
        }
    }

    Result initialize(const cv::Mat& frame, const cv::Rect2d& roi)
    {
        center = {float(roi.x + (roi.width-1)*0.5), float(roi.y + (roi.height-1)*0.5)};
        size = {float(roi.width), float(roi.height)};
        average = cv::mean(frame);
        auto pixels = crop(frame, std::lround(padded_size(size.width, size.height)), 127);
        template_net.run({pixels.data()});
        return {roi, 0, true};
    }

    Result track(const cv::Mat& frame)
    {
        const float template_size = padded_size(size.width, size.height);
        const float scale = 127.f / template_size;
        auto pixels = crop(frame, std::lround(template_size * 255.f / 127.f), 255);
        search_net.run({pixels.data()});
        head.run({template_net.output(0), search_net.output(0)});
        const auto* scores = head.output(0);
        const auto* boxes = head.output(1);
        float best_rank = -1, best_score = 0, best_penalty = 0;
        cv::Point2f offset;
        cv::Size2f proposed;
        for (int i = 0; i < 225; ++i) {
            const float bg = scores[i], fg = scores[225+i];
            if (!std::isfinite(bg) || !std::isfinite(fg))
                throw std::runtime_error("Non-finite classification output");
            const float maximum = std::max(bg, fg);
            const float e0 = std::exp(bg-maximum), e1 = std::exp(fg-maximum);
            const float score = e1 / (e0+e1);
            const float x1 = (i%15-7)*16.f - boxes[i];
            const float y1 = (i/15-7)*16.f - boxes[225+i];
            const float x2 = (i%15-7)*16.f + boxes[450+i];
            const float y2 = (i/15-7)*16.f + boxes[675+i];
            const float w = x2-x1, h = y2-y1;
            if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) ||
                !std::isfinite(y2) || !std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0)
                throw std::runtime_error("Invalid localization output");
            const float sr = padded_size(w, h) / padded_size(size.width*scale, size.height*scale);
            const float rr = (size.width/size.height)/(w/h);
            const float penalty = std::exp(-(std::max(sr, 1.f/sr)*std::max(rr, 1.f/rr)-1)*0.138f);
            const float rank = penalty * score * (1-0.455f) + window[i]*0.455f;
            if (!std::isfinite(rank)) throw std::runtime_error("Invalid tracking rank");
            if (rank > best_rank) {
                best_rank = rank; best_score = score; best_penalty = penalty;
                offset = {(x1+x2)*0.5f/scale, (y1+y2)*0.5f/scale};
                proposed = {w/scale, h/scale};
            }
        }
        if (!std::isfinite(offset.x) || !std::isfinite(offset.y) ||
            !std::isfinite(proposed.width) || !std::isfinite(proposed.height))
            throw std::runtime_error("Invalid tracking coordinates");
        const float lr = best_penalty * best_score * 0.348f;
        center.x = std::clamp(center.x+offset.x, 0.f, float(frame.cols));
        center.y = std::clamp(center.y+offset.y, 0.f, float(frame.rows));
        size.width = std::clamp(size.width*(1-lr)+proposed.width*lr, 10.f, float(frame.cols));
        size.height = std::clamp(size.height*(1-lr)+proposed.height*lr, 10.f, float(frame.rows));
        return {{center.x-size.width/2, center.y-size.height/2, size.width, size.height}, best_score, false};
    }

private:
    std::vector<float> crop(const cv::Mat& frame, int original, int output)
    {
        if (original <= 0 || original > 16384)
            throw std::runtime_error("Unsupported crop size");
        const int x = static_cast<int>(std::floor(center.x-original*0.5f));
        const int y = static_cast<int>(std::floor(center.y-original*0.5f));
        cv::Mat padded(original, original, CV_8UC3, average);
        const cv::Rect valid = cv::Rect(x, y, original, original) & cv::Rect(0, 0, frame.cols, frame.rows);
        if (valid.empty()) throw std::runtime_error("Crop outside frame");
        frame(valid).copyTo(padded(cv::Rect(valid.x-x, valid.y-y, valid.width, valid.height)));
        cv::Mat resized, floats;
        cv::resize(padded, resized, {output, output}, 0, 0, cv::INTER_LINEAR);
        resized.convertTo(floats, CV_32FC3); // BGR 0–255, no normalization or RGB swap.
        std::vector<float> pixels(size_t(output)*output*3);
        cv::Mat planes[] = {
            cv::Mat(output, output, CV_32F, pixels.data()),
            cv::Mat(output, output, CV_32F, pixels.data()+output*output),
            cv::Mat(output, output, CV_32F, pixels.data()+2*output*output)
        };
        cv::split(floats, planes); // Interleaved HWC to contiguous NCHW, batch size 1.
        return pixels;
    }
    // Sessions and output tensors are destroyed before their environment.
    Ort::Env env;
    Ort::SessionOptions options;
    Model template_net, search_net, head;
    cv::Point2f center;
    cv::Size2f size;
    cv::Scalar average;
    std::array<float, 225> window{};
};

struct State {
    // ponytail: per-instance lock covers inference; snapshot settings if setter latency matters.
    std::mutex mutex;
    std::string roi, models_dir;
    bool enabled = false, started = false, reset = true;
    GstVideoInfo info{};
    std::unique_ptr<Tracker> tracker;
};
} // namespace

typedef struct _GstCpuNanoTrack {
    GstBaseTransform parent;
    State* state;
} GstCpuNanoTrack;
typedef struct _GstCpuNanoTrackClass { GstBaseTransformClass parent_class; } GstCpuNanoTrackClass;
G_DEFINE_TYPE(GstCpuNanoTrack, gst_cpu_nano_track, GST_TYPE_BASE_TRANSFORM)

enum { PROP_0, PROP_ENABLED, PROP_ROI, PROP_MODELS_DIR };
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=BGR,width=[10,16384],height=[10,16384],interlace-mode=progressive"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=BGR,width=[10,16384],height=[10,16384],interlace-mode=progressive"));

static void set_property(GObject* object, guint id, const GValue* value, GParamSpec* spec)
{
    auto* self = reinterpret_cast<GstCpuNanoTrack*>(object);
    try {
        auto& s = *self->state;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (id == PROP_MODELS_DIR && s.started) {
            GST_WARNING_OBJECT(self, "%s can only change in NULL or READY", spec->name);
            return;
        }
        const auto text = [&]() { const char* p = g_value_get_string(value); return p ? p : ""; };
        switch (id) {
        case PROP_ENABLED:
            if (s.enabled != bool(g_value_get_boolean(value))) s.reset = true;
            s.enabled = g_value_get_boolean(value); break;
        case PROP_ROI: s.roi = text(); s.reset = true; break;
        case PROP_MODELS_DIR: s.models_dir = text(); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
        }
    } catch (const std::exception& e) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, ("Cannot set tracker property"), ("%s", e.what()));
    }
}
static void get_property(GObject* object, guint id, GValue* value, GParamSpec* spec)
{
    auto& s = *reinterpret_cast<GstCpuNanoTrack*>(object)->state;
    std::lock_guard<std::mutex> lock(s.mutex);
    switch (id) {
    case PROP_ENABLED: g_value_set_boolean(value, s.enabled); break;
    case PROP_ROI: g_value_set_string(value, s.roi.c_str()); break;
    case PROP_MODELS_DIR: g_value_set_string(value, s.models_dir.c_str()); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}
static gboolean start(GstBaseTransform* base)
{
    auto& s = *reinterpret_cast<GstCpuNanoTrack*>(base)->state;
    std::lock_guard<std::mutex> lock(s.mutex);
    try {
        if (!s.models_dir.empty()) {
            const auto begin = std::chrono::steady_clock::now();
            s.tracker = std::make_unique<Tracker>(s.models_dir);
            const double milliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();
            GST_INFO_OBJECT(base, "Loaded NanoTrack ONNX models from %s in %.3f ms",
                            s.models_dir.c_str(), milliseconds);
        }
        s.reset = true;
        s.started = true;
        return TRUE;
    } catch (const std::exception& error) {
        s.tracker.reset();
        GST_ELEMENT_ERROR(base, RESOURCE, OPEN_READ,
                          ("Failed to load NanoTrack ONNX models"),
                          ("%s", error.what()));
        return FALSE;
    }
}
static gboolean stop(GstBaseTransform* base)
{
    auto& s = *reinterpret_cast<GstCpuNanoTrack*>(base)->state;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.tracker.reset();
    s.reset = true;
    s.started = false;
    return TRUE;
}
static gboolean set_caps(GstBaseTransform* base, GstCaps* input, GstCaps*)
{
    auto& s = *reinterpret_cast<GstCpuNanoTrack*>(base)->state;
    std::lock_guard<std::mutex> lock(s.mutex);
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, input) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGR ||
        GST_VIDEO_INFO_IS_INTERLACED(&info)) return FALSE;
    if (GST_VIDEO_INFO_WIDTH(&s.info) != GST_VIDEO_INFO_WIDTH(&info) ||
        GST_VIDEO_INFO_HEIGHT(&s.info) != GST_VIDEO_INFO_HEIGHT(&info)) s.reset = true;
    s.info = info;
    return TRUE;
}
static gboolean sink_event(GstBaseTransform* base, GstEvent* event)
{
    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_STREAM_START: case GST_EVENT_SEGMENT: case GST_EVENT_FLUSH_STOP: {
        auto& s = *reinterpret_cast<GstCpuNanoTrack*>(base)->state;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.reset = true;
        break;
    }
    default: break;
    }
    return GST_BASE_TRANSFORM_CLASS(gst_cpu_nano_track_parent_class)->sink_event(base, event);
}
static GstFlowReturn transform_ip(GstBaseTransform* base, GstBuffer* buffer)
{
    auto& s = *reinterpret_cast<GstCpuNanoTrack*>(base)->state;
    GstVideoFrame mapped{};
    bool is_mapped = false;
    try {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) s.reset = true;
        if (!s.enabled) return GST_FLOW_OK;
        if (!gst_video_frame_map(&mapped, &s.info, buffer, GST_MAP_READ))
            throw std::runtime_error("Cannot map BGR frame");
        is_mapped = true;
        const int width = GST_VIDEO_FRAME_WIDTH(&mapped), height = GST_VIDEO_FRAME_HEIGHT(&mapped);
        const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0);
        if (width < 10 || height < 10 || stride < width*3)
            throw std::runtime_error("Invalid BGR dimensions or stride");
        cv::Mat image(height, width, CV_8UC3, GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0), stride);
        cv::Rect2d roi;
        if (s.reset) roi = parse_roi(s.roi, width, height);
        if (!s.tracker) {
            if (s.models_dir.empty()) throw std::runtime_error("models-dir is required when enabled");
            s.tracker = std::make_unique<Tracker>(s.models_dir);
        }
        Result result = s.reset ? s.tracker->initialize(image, roi) :
                                 s.tracker->track(image);
        s.reset = false;
        gst_video_frame_unmap(&mapped);
        is_mapped = false;
        const auto& b = result.box;
        const int x = std::clamp(int(std::floor(b.x)), 0, width);
        const int y = std::clamp(int(std::floor(b.y)), 0, height);
        const int right = std::clamp(int(std::ceil(b.x+b.width)), 0, width);
        const int bottom = std::clamp(int(std::ceil(b.y+b.height)), 0, height);
        auto* meta = gst_buffer_add_video_region_of_interest_meta(buffer, "nanotrack", x, y, right-x, bottom-y);
        if (!meta) throw std::runtime_error("Cannot attach ROI metadata");
        auto* parameters = gst_structure_new("nanotrack", "initialized", G_TYPE_BOOLEAN, result.initialized, nullptr);
        if (!result.initialized)
            gst_structure_set(parameters, "confidence", G_TYPE_DOUBLE, result.confidence, nullptr);
        gst_video_region_of_interest_meta_add_param(meta, parameters);
        GST_LOG_OBJECT(base, "initialized=%d box=%d,%d,%d,%d confidence=%.6f", result.initialized,
                       x, y, right-x, bottom-y, result.confidence);
        return GST_FLOW_OK;
    } catch (const std::exception& e) {
        if (is_mapped) gst_video_frame_unmap(&mapped);
        GST_ELEMENT_ERROR(base, STREAM, FAILED, ("NanoTrack frame processing failed"), ("%s", e.what()));
        return GST_FLOW_ERROR;
    } catch (...) {
        if (is_mapped) gst_video_frame_unmap(&mapped);
        GST_ELEMENT_ERROR(base, STREAM, FAILED, ("Unknown NanoTrack processing failure"), (nullptr));
        return GST_FLOW_ERROR;
    }
}
static void finalize(GObject* object)
{
    delete reinterpret_cast<GstCpuNanoTrack*>(object)->state;
    G_OBJECT_CLASS(gst_cpu_nano_track_parent_class)->finalize(object);
}
static void gst_cpu_nano_track_class_init(GstCpuNanoTrackClass* klass)
{
    auto* object = G_OBJECT_CLASS(klass);
    auto* element = GST_ELEMENT_CLASS(klass);
    auto* transform = GST_BASE_TRANSFORM_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    const auto live = GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING);
    const auto ready = GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(object, PROP_ENABLED,
        g_param_spec_boolean("enabled", "Enabled", "Track the configured ROI; enabling captures a fresh template", FALSE, live));
    g_object_class_install_property(object, PROP_ROI,
        g_param_spec_string("roi", "ROI", "Initial x,y,width,height in frame pixels; assignment resets tracking", "", live));
    g_object_class_install_property(object, PROP_MODELS_DIR,
        g_param_spec_string("models-dir", "Models directory", "Directory containing NanoTrack CPU ONNX models", "", ready));
    gst_element_class_set_static_metadata(element, "CPU ONNX NanoTrack", "Filter/Metadata/Video",
        "Tracks one ROI with NanoTrackV3 and attaches ROI metadata", "example");
    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);
    transform->start = GST_DEBUG_FUNCPTR(start);
    transform->stop = GST_DEBUG_FUNCPTR(stop);
    transform->set_caps = GST_DEBUG_FUNCPTR(set_caps);
    transform->sink_event = GST_DEBUG_FUNCPTR(sink_event);
    transform->transform_ip = GST_DEBUG_FUNCPTR(transform_ip);
}
static void gst_cpu_nano_track_init(GstCpuNanoTrack* self)
{
    self->state = new State;
    gst_video_info_init(&self->state->info);
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
}
static gboolean plugin_init(GstPlugin* plugin)
{
    GST_DEBUG_CATEGORY_INIT(nanotrack_debug, "cpunanotrack", 0, "CPU ONNX NanoTrack");
    return gst_element_register(plugin, "cpunanotrack", GST_RANK_NONE, gst_cpu_nano_track_get_type());
}
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, cpunanotrack,
    "NanoTrackV3 CPU ONNX tracking metadata", plugin_init, "1.0", "LGPL", "cpunanotrack", "https://example.com")
