// CPU LightTrack-Mobile ONNX element. Its GStreamer contract intentionally mirrors cpunanotrack.
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <opencv2/imgproc.hpp>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PACKAGE
#define PACKAGE "cpulighttrack"
#endif
GST_DEBUG_CATEGORY_STATIC(lighttrack_debug);
#define GST_CAT_DEFAULT lighttrack_debug

namespace {
constexpr int kTemplateSize = 128, kSearchSize = 256, kScoreSize = 16, kStride = 16;

cv::Rect2d parse_roi(const std::string& text, int width, int height)
{
    cv::Rect2d roi; char a, b, c; std::istringstream input(text);
    if (!(input >> roi.x >> a >> roi.y >> b >> roi.width >> c >> roi.height) || a != ',' || b != ',' || c != ',' ||
        !(input >> std::ws).eof() || !std::isfinite(roi.x) || !std::isfinite(roi.y) || !std::isfinite(roi.width) ||
        !std::isfinite(roi.height) || roi.x < 0 || roi.y < 0 || roi.width < 1 || roi.height < 1 ||
        roi.x + roi.width > width || roi.y + roi.height > height)
        throw std::runtime_error("roi must be finite x,y,width,height inside the frame, with dimensions >= 1");
    return roi;
}

class Model {
public:
    Model(Ort::Env& env, const Ort::SessionOptions& options, const std::string& path,
          std::vector<std::vector<int64_t>> inputs, std::vector<std::vector<int64_t>> outputs)
        : session(env, path.c_str(), options), inputs(std::move(inputs)), outputs(std::move(outputs))
    {
        if (session.GetInputCount() != this->inputs.size() || session.GetOutputCount() != this->outputs.size())
            throw std::runtime_error("Unexpected LightTrack ONNX I/O count: " + path);
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < this->inputs.size(); ++i) {
            validate(session.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo(), this->inputs[i]);
            input_names.emplace_back(session.GetInputNameAllocated(i, allocator).get());
        }
        for (size_t i = 0; i < this->outputs.size(); ++i) {
            validate(session.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo(), this->outputs[i]);
            output_names.emplace_back(session.GetOutputNameAllocated(i, allocator).get());
        }
        for (const auto& name : input_names) input_ptrs.push_back(name.c_str());
        for (const auto& name : output_names) output_ptrs.push_back(name.c_str());
    }
    void run(std::initializer_list<float*> buffers)
    {
        if (buffers.size() != inputs.size()) throw std::runtime_error("Incorrect ONNX input count");
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> tensors;
        size_t index = 0;
        for (float* data : buffers) {
            const auto& shape = inputs[index++];
            const auto count = std::accumulate(shape.begin(), shape.end(), size_t(1), std::multiplies<size_t>());
            tensors.push_back(Ort::Value::CreateTensor<float>(memory, data, count, shape.data(), shape.size()));
        }
        values = session.Run(Ort::RunOptions{nullptr}, input_ptrs.data(), tensors.data(), tensors.size(),
                             output_ptrs.data(), output_ptrs.size());
        for (size_t i = 0; i < values.size(); ++i) validate(values[i].GetTensorTypeAndShapeInfo(), outputs[i]);
    }
    float* output(size_t index) { return values.at(index).GetTensorMutableData<float>(); }
private:
    template<typename Info>
    static void validate(const Info& info, const std::vector<int64_t>& shape)
    {
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetShape() != shape)
            throw std::runtime_error("Expected fixed LightTrack FLOAT32 NCHW tensor shape");
    }
    Ort::Session session;
    std::vector<std::vector<int64_t>> inputs, outputs;
    std::vector<std::string> input_names, output_names;
    std::vector<const char*> input_ptrs, output_ptrs;
    std::vector<Ort::Value> values;
};

Ort::SessionOptions cpu_options()
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    return options;
}

float padded_size(float width, float height)
{
    const float pad = (width + height) * .5f;
    return std::sqrt((width + pad) * (height + pad));
}
float change(float value) { return std::max(value, 1.f / value); }

struct Result { cv::Rect2d box; double confidence = 0; bool initialized = false; };

class Tracker {
public:
    explicit Tracker(const std::string& dir)
        : env(ORT_LOGGING_LEVEL_WARNING, "cpulighttrack"), options(cpu_options()),
          template_net(env, options, dir + "/lighttrack_template.onnx", {{1,3,128,128}}, {{1,96,8,8}}),
          search_net(env, options, dir + "/lighttrack_search.onnx", {{1,3,256,256}}, {{1,96,16,16}}),
          head(env, options, dir + "/lighttrack_head.onnx", {{1,96,8,8}, {1,96,16,16}}, {{1,1,16,16}, {1,4,16,16}})
    {
        for (int y = 0, i = 0; y < kScoreSize; ++y)
            for (int x = 0; x < kScoreSize; ++x, ++i)
                window[i] = (.5f - .5f * std::cos(2.f * float(CV_PI) * x / (kScoreSize - 1))) *
                            (.5f - .5f * std::cos(2.f * float(CV_PI) * y / (kScoreSize - 1)));
    }

    Result initialize(const cv::Mat& frame, const cv::Rect2d& roi)
    {
        center = {float(roi.x + roi.width * .5), float(roi.y + roi.height * .5)};
        size = {float(roi.width), float(roi.height)};
        average = cv::mean(frame);
        const auto wc = size.width + .5f * (size.width + size.height);
        const auto hc = size.height + .5f * (size.width + size.height);
        auto pixels = crop(frame, int(std::round(std::sqrt(wc * hc))), kTemplateSize);
        template_net.run({pixels.data()});
        return {roi, 0, true};
    }

    Result track(const cv::Mat& frame)
    {
        const auto wc = size.width + .5f * (size.width + size.height);
        const auto hc = size.height + .5f * (size.width + size.height);
        const auto s_z = std::sqrt(wc * hc);
        const auto scale = float(kTemplateSize) / s_z;
        const auto pad = float(kSearchSize - kTemplateSize) / (2.f * scale);
        auto pixels = crop(frame, int(std::round(s_z + 2.f * pad)), kSearchSize);
        search_net.run({pixels.data()});
        head.run({template_net.output(0), search_net.output(0)});
        const auto* scores = head.output(0);
        const auto* boxes = head.output(1);
        float best_rank = -1, best_score = 0, best_penalty = 0;
        cv::Point2f best_center; cv::Size2f best_size;
        for (int i = 0; i < kScoreSize * kScoreSize; ++i) {
            const float score = 1.f / (1.f + std::exp(-scores[i]));
            const float grid_x = (i % kScoreSize - kScoreSize / 2) * kStride + kSearchSize / 2;
            const float grid_y = (i / kScoreSize - kScoreSize / 2) * kStride + kSearchSize / 2;
            const float x1 = grid_x - boxes[i], y1 = grid_y - boxes[256 + i];
            const float x2 = grid_x + boxes[512 + i], y2 = grid_y + boxes[768 + i];
            const float width = x2 - x1, height = y2 - y1;
            if (!std::isfinite(score) || width <= 0 || height <= 0) throw std::runtime_error("Invalid LightTrack output");
            const float size_ratio = change(padded_size(width, height) / padded_size(size.width * scale, size.height * scale));
            const float ratio = change((size.width / size.height) / (width / height));
            const float penalty = std::exp(-(size_ratio * ratio - 1.f) * .007f);
            const float rank = penalty * score * .775f + window[i] * .225f;
            if (rank > best_rank) {
                best_rank = rank; best_score = score; best_penalty = penalty;
                best_center = {(x1 + x2) * .5f, (y1 + y2) * .5f}; best_size = {width, height};
            }
        }
        const float lr = best_penalty * best_score * .616f;
        center.x = std::clamp(center.x + (best_center.x - kSearchSize * .5f) / scale, 0.f, float(frame.cols));
        center.y = std::clamp(center.y + (best_center.y - kSearchSize * .5f) / scale, 0.f, float(frame.rows));
        size.width = std::clamp(size.width * (1.f - lr) + best_size.width / scale * lr, 10.f, float(frame.cols));
        size.height = std::clamp(size.height * (1.f - lr) + best_size.height / scale * lr, 10.f, float(frame.rows));
        return {{center.x - size.width * .5, center.y - size.height * .5, size.width, size.height}, best_score, false};
    }

private:
    std::vector<float> crop(const cv::Mat& frame, int original, int output)
    {
        if (original <= 0 || original > 16384) throw std::runtime_error("Unsupported crop size");
        const int x = int(std::round(center.x - (original + 1) * .5f));
        const int y = int(std::round(center.y - (original + 1) * .5f));
        cv::Mat padded(original, original, CV_8UC3, average);
        const cv::Rect valid = cv::Rect(x, y, original, original) & cv::Rect(0, 0, frame.cols, frame.rows);
        if (valid.empty()) throw std::runtime_error("Crop outside frame");
        frame(valid).copyTo(padded(cv::Rect(valid.x - x, valid.y - y, valid.width, valid.height)));
        cv::Mat resized, rgb, floats;
        cv::resize(padded, resized, {output, output}, 0, 0, cv::INTER_LINEAR);
        cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
        rgb.convertTo(floats, CV_32FC3, 1. / 255.);
        std::vector<float> pixels(size_t(output) * output * 3);
        cv::Mat planes[] = {cv::Mat(output, output, CV_32F, pixels.data()),
                            cv::Mat(output, output, CV_32F, pixels.data() + output * output),
                            cv::Mat(output, output, CV_32F, pixels.data() + 2 * output * output)};
        cv::split(floats, planes);
        constexpr std::array<float, 3> mean{.485f, .456f, .406f}, stddev{.229f, .224f, .225f};
        for (int channel = 0; channel < 3; ++channel)
            for (int i = 0; i < output * output; ++i)
                pixels[channel * output * output + i] = (pixels[channel * output * output + i] - mean[channel]) / stddev[channel];
        return pixels;
    }
    Ort::Env env; Ort::SessionOptions options; Model template_net, search_net, head;
    cv::Point2f center; cv::Size2f size; cv::Scalar average; std::array<float, 256> window{};
};

struct State {
    // ponytail: one lock keeps property changes and inference consistent; split it only if profiling needs it.
    std::mutex mutex; std::string roi, models_dir; bool enabled = false, started = false, reset = true;
    GstVideoInfo info{}; std::unique_ptr<Tracker> tracker;
};
} // namespace

typedef struct _GstCpuLightTrack { GstBaseTransform parent; State* state; } GstCpuLightTrack;
typedef struct _GstCpuLightTrackClass { GstBaseTransformClass parent_class; } GstCpuLightTrackClass;
G_DEFINE_TYPE(GstCpuLightTrack, gst_cpu_light_track, GST_TYPE_BASE_TRANSFORM)
enum { PROP_0, PROP_ENABLED, PROP_ROI, PROP_MODELS_DIR };
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=BGR,width=[10,16384],height=[10,16384],interlace-mode=progressive"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=BGR,width=[10,16384],height=[10,16384],interlace-mode=progressive"));

static void set_property(GObject* object, guint id, const GValue* value, GParamSpec* spec)
{
    auto* self = reinterpret_cast<GstCpuLightTrack*>(object); auto& state = *self->state;
    std::lock_guard<std::mutex> lock(state.mutex);
    switch (id) {
    case PROP_ENABLED: if (state.enabled != bool(g_value_get_boolean(value))) state.reset = true; state.enabled = g_value_get_boolean(value); break;
    case PROP_ROI: { const char* text = g_value_get_string(value); state.roi = text ? text : ""; state.reset = true; break; }
    case PROP_MODELS_DIR: { const char* text = g_value_get_string(value); if (state.started) GST_WARNING_OBJECT(self, "models-dir can only change in NULL or READY"); else state.models_dir = text ? text : ""; break; }
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}
static void get_property(GObject* object, guint id, GValue* value, GParamSpec* spec)
{
    auto& state = *reinterpret_cast<GstCpuLightTrack*>(object)->state; std::lock_guard<std::mutex> lock(state.mutex);
    if (id == PROP_ENABLED) g_value_set_boolean(value, state.enabled);
    else if (id == PROP_ROI) g_value_set_string(value, state.roi.c_str());
    else if (id == PROP_MODELS_DIR) g_value_set_string(value, state.models_dir.c_str());
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static gboolean start(GstBaseTransform* base)
{
    auto& state = *reinterpret_cast<GstCpuLightTrack*>(base)->state; std::lock_guard<std::mutex> lock(state.mutex);
    try { if (!state.models_dir.empty()) state.tracker = std::make_unique<Tracker>(state.models_dir); state.reset = true; state.started = true; return TRUE; }
    catch (const std::exception& e) { state.tracker.reset(); GST_ELEMENT_ERROR(base, RESOURCE, OPEN_READ, ("Failed to load LightTrack ONNX models"), ("%s", e.what())); return FALSE; }
}
static gboolean stop(GstBaseTransform* base)
{ auto& state = *reinterpret_cast<GstCpuLightTrack*>(base)->state; std::lock_guard<std::mutex> lock(state.mutex); state.tracker.reset(); state.reset = true; state.started = false; return TRUE; }
static gboolean set_caps(GstBaseTransform* base, GstCaps* caps, GstCaps*)
{
    auto& state = *reinterpret_cast<GstCpuLightTrack*>(base)->state; std::lock_guard<std::mutex> lock(state.mutex); GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGR || GST_VIDEO_INFO_IS_INTERLACED(&info)) return FALSE;
    if (GST_VIDEO_INFO_WIDTH(&state.info) != GST_VIDEO_INFO_WIDTH(&info) || GST_VIDEO_INFO_HEIGHT(&state.info) != GST_VIDEO_INFO_HEIGHT(&info)) state.reset = true;
    state.info = info; return TRUE;
}
static gboolean sink_event(GstBaseTransform* base, GstEvent* event)
{
    if (GST_EVENT_TYPE(event) == GST_EVENT_STREAM_START || GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT || GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
        auto& state = *reinterpret_cast<GstCpuLightTrack*>(base)->state; std::lock_guard<std::mutex> lock(state.mutex); state.reset = true;
    }
    return GST_BASE_TRANSFORM_CLASS(gst_cpu_light_track_parent_class)->sink_event(base, event);
}
static GstFlowReturn transform_ip(GstBaseTransform* base, GstBuffer* buffer)
{
    auto& state = *reinterpret_cast<GstCpuLightTrack*>(base)->state; GstVideoFrame mapped{}; bool mapped_ok = false;
    try {
        std::lock_guard<std::mutex> lock(state.mutex); if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) state.reset = true;
        if (!state.enabled) return GST_FLOW_OK;
        if (!gst_video_frame_map(&mapped, &state.info, buffer, GST_MAP_READ))
            throw std::runtime_error("Cannot map BGR frame");
        mapped_ok = true;
        const int width = GST_VIDEO_FRAME_WIDTH(&mapped), height = GST_VIDEO_FRAME_HEIGHT(&mapped), stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0);
        if (stride < width * 3) throw std::runtime_error("Invalid BGR stride");
        cv::Mat image(height, width, CV_8UC3, GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0), stride);
        if (!state.tracker) { if (state.models_dir.empty()) throw std::runtime_error("models-dir is required when enabled"); state.tracker = std::make_unique<Tracker>(state.models_dir); }
        const auto result = state.reset ? state.tracker->initialize(image, parse_roi(state.roi, width, height)) : state.tracker->track(image); state.reset = false;
        gst_video_frame_unmap(&mapped); mapped_ok = false;
        const int x = std::clamp(int(std::floor(result.box.x)), 0, width), y = std::clamp(int(std::floor(result.box.y)), 0, height);
        const int right = std::clamp(int(std::ceil(result.box.x + result.box.width)), 0, width), bottom = std::clamp(int(std::ceil(result.box.y + result.box.height)), 0, height);
        auto* meta = gst_buffer_add_video_region_of_interest_meta(buffer, "lighttrack", x, y, right - x, bottom - y);
        if (!meta) throw std::runtime_error("Cannot attach ROI metadata");
        auto* parameters = gst_structure_new("lighttrack", "initialized", G_TYPE_BOOLEAN, result.initialized, nullptr);
        if (!result.initialized) gst_structure_set(parameters, "confidence", G_TYPE_DOUBLE, result.confidence, nullptr);
        gst_video_region_of_interest_meta_add_param(meta, parameters); return GST_FLOW_OK;
    } catch (const std::exception& e) { if (mapped_ok) gst_video_frame_unmap(&mapped); GST_ELEMENT_ERROR(base, STREAM, FAILED, ("LightTrack frame processing failed"), ("%s", e.what())); return GST_FLOW_ERROR; }
}
static void finalize(GObject* object) { delete reinterpret_cast<GstCpuLightTrack*>(object)->state; G_OBJECT_CLASS(gst_cpu_light_track_parent_class)->finalize(object); }
static void gst_cpu_light_track_class_init(GstCpuLightTrackClass* klass)
{
    auto* object = G_OBJECT_CLASS(klass); auto* element = GST_ELEMENT_CLASS(klass); auto* transform = GST_BASE_TRANSFORM_CLASS(klass);
    object->set_property = set_property; object->get_property = get_property; object->finalize = finalize;
    const auto live = GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING), ready = GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(object, PROP_ENABLED, g_param_spec_boolean("enabled", "Enabled", "Track the configured ROI", FALSE, live));
    g_object_class_install_property(object, PROP_ROI, g_param_spec_string("roi", "ROI", "Initial x,y,width,height in frame pixels", "", live));
    g_object_class_install_property(object, PROP_MODELS_DIR, g_param_spec_string("models-dir", "Models directory", "Directory containing LightTrack ONNX models", "", ready));
    gst_element_class_set_static_metadata(element, "CPU ONNX LightTrack", "Filter/Metadata/Video", "Tracks one ROI with LightTrack-Mobile and attaches ROI metadata", "example");
    gst_element_class_add_static_pad_template(element, &sink_template); gst_element_class_add_static_pad_template(element, &src_template);
    transform->start = GST_DEBUG_FUNCPTR(start); transform->stop = GST_DEBUG_FUNCPTR(stop); transform->set_caps = GST_DEBUG_FUNCPTR(set_caps); transform->sink_event = GST_DEBUG_FUNCPTR(sink_event); transform->transform_ip = GST_DEBUG_FUNCPTR(transform_ip);
}
static void gst_cpu_light_track_init(GstCpuLightTrack* self) { self->state = new State; gst_video_info_init(&self->state->info); gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE); gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE); }
static gboolean plugin_init(GstPlugin* plugin) { GST_DEBUG_CATEGORY_INIT(lighttrack_debug, "cpulighttrack", 0, "CPU ONNX LightTrack"); return gst_element_register(plugin, "cpulighttrack", GST_RANK_NONE, gst_cpu_light_track_get_type()); }
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, cpulighttrack, "LightTrack-Mobile CPU ONNX tracking metadata", plugin_init, "1.0", "LGPL", "cpulighttrack", "https://example.com")
