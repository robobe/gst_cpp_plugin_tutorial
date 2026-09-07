// NanoTrackV3 tracking math and shared RKNN buffers adapted from
// radxa/nano/nanotrack_rknn_rga.cpp. GStreamer owns capture and buffer lifetime.
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <opencv2/imgproc.hpp>
#include <rknn_api.h>
#include <rga/im2d.h>
#include <rga/rga.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PACKAGE
#define PACKAGE "rknnnanotrack"
#endif
GST_DEBUG_CATEGORY_STATIC(nanotrack_debug);
#define GST_CAT_DEFAULT nanotrack_debug

namespace {
void check(int status, const std::string& operation)
{
    if (status < 0)
        throw std::runtime_error(operation + ": RKNN status " + std::to_string(status));
}

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

std::string model_path(const std::string& dir, const std::string& precision,
                       const char* stem, bool head)
{
    if (dir.empty()) throw std::runtime_error("models-dir is required when enabled");
    std::string suffix;
    if (precision == "int8") suffix = "_int8";
    else if (precision == "int8-mmse") suffix = "_int8_mmse";
    else if (precision == "mixed" && head) suffix = "_int8";
    else if (precision != "fp16" && precision != "mixed")
        throw std::runtime_error("precision must be fp16, mixed, int8, or int8-mmse");
    return dir + "/" + stem + suffix + ".rknn";
}

class Model {
public:
    explicit Model(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("Cannot open model: " + path);
        const auto length = file.tellg();
        if (length <= 0 || static_cast<uint64_t>(length) > UINT32_MAX)
            throw std::runtime_error("Invalid model file size: " + path);
        std::vector<char> bytes(static_cast<size_t>(length));
        file.seekg(0);
        if (!file.read(bytes.data(), length)) throw std::runtime_error("Cannot read model: " + path);
        check(rknn_init(&ctx, bytes.data(), static_cast<uint32_t>(bytes.size()), 0, nullptr), "load " + path);
        try {
            rknn_input_output_num counts{};
            check(rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &counts, sizeof(counts)), "query counts");
            inputs.resize(counts.n_input);
            outputs.resize(counts.n_output);
            for (uint32_t i = 0; i < counts.n_input; ++i) {
                inputs[i].index = i;
                check(rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &inputs[i], sizeof(inputs[i])), "query input");
            }
            for (uint32_t i = 0; i < counts.n_output; ++i) {
                outputs[i].index = i;
                check(rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &outputs[i], sizeof(outputs[i])), "query output");
            }
        } catch (...) { release(); throw; }
    }
    ~Model() { release(); }
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    static void shape(const rknn_tensor_attr& attr, uint32_t c, uint32_t h, uint32_t w)
    {
        const bool nchw = attr.fmt == RKNN_TENSOR_NCHW && attr.dims[1] == c &&
            attr.dims[2] == h && attr.dims[3] == w;
        const bool nhwc = attr.fmt == RKNN_TENSOR_NHWC && attr.dims[1] == h &&
            attr.dims[2] == w && attr.dims[3] == c;
        if (attr.n_dims != 4 || attr.dims[0] != 1 || (!nchw && !nhwc) || attr.n_elems != c*h*w)
            throw std::runtime_error("Unexpected NanoTrack tensor shape/layout: " + std::string(attr.name));
    }

    rknn_tensor_mem* allocate(rknn_tensor_attr attr)
    {
        auto* memory = rknn_create_mem(ctx, attr.size_with_stride);
        if (!memory) throw std::runtime_error("rknn_create_mem failed");
        own(memory);
        if (!memory->virt_addr) throw std::runtime_error("RKNN memory is not CPU mappable");
        check(rknn_set_io_mem(ctx, memory, &attr), "bind tensor memory");
        return memory;
    }

    rknn_tensor_mem* image(int side)
    {
        auto attr = inputs.at(0);
        const int aligned = (side + 15) & ~15;
        attr.type = RKNN_TENSOR_UINT8;
        attr.fmt = RKNN_TENSOR_NHWC;
        attr.pass_through = 0;
        attr.size = side * side * 3;
        attr.size_with_stride = aligned * aligned * 3;
        attr.w_stride = aligned;
        attr.h_stride = aligned;
        auto* memory = allocate(attr);
        std::memset(memory->virt_addr, 0, memory->size);
        return memory;
    }

    rknn_tensor_mem* floats(int index, rknn_tensor_format format)
    {
        auto attr = outputs.at(index);
        attr.type = RKNN_TENSOR_FLOAT32;
        attr.fmt = format;
        attr.size = attr.n_elems * sizeof(float);
        attr.size_with_stride = attr.size;
        return allocate(attr);
    }

    void import(int index, rknn_tensor_mem* source)
    {
        auto attr = inputs.at(index);
        attr.type = RKNN_TENSOR_FLOAT32;
        attr.fmt = RKNN_TENSOR_NHWC;
        attr.pass_through = 0;
        attr.size = attr.n_elems * sizeof(float);
        attr.size_with_stride = attr.size;
        if (source->size < attr.size) throw std::runtime_error("Feature buffer too small");
        auto* memory = rknn_create_mem_from_fd(ctx, source->fd, source->virt_addr, attr.size, source->offset);
        if (!memory) throw std::runtime_error("Cannot import shared feature memory");
        own(memory);
        check(rknn_set_io_mem(ctx, memory, &attr), "bind head feature input");
    }
    void run() { check(rknn_run(ctx, nullptr), "run model"); }
    void sync(rknn_tensor_mem* memory, rknn_mem_sync_mode mode)
    { check(rknn_mem_sync(ctx, memory, mode), "synchronize tensor memory"); }

    rknn_context ctx = 0;
    std::vector<rknn_tensor_attr> inputs, outputs;
private:
    void own(rknn_tensor_mem* memory)
    {
        try { memories.push_back(memory); }
        catch (...) { rknn_destroy_mem(ctx, memory); throw; }
    }
    void release() noexcept
    {
        for (auto* memory : memories) rknn_destroy_mem(ctx, memory);
        if (ctx) rknn_destroy(ctx);
        memories.clear();
        ctx = 0;
    }
    std::vector<rknn_tensor_mem*> memories;
};

struct RgaBuffer {
    explicit RgaBuffer(rga_buffer_handle_t value) : handle(value) {}
    ~RgaBuffer() { if (handle) releasebuffer_handle(handle); }
    RgaBuffer(const RgaBuffer&) = delete;
    RgaBuffer& operator=(const RgaBuffer&) = delete;
    rga_buffer_handle_t handle;
};

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
    Tracker(const std::string& dir, const std::string& precision, bool rga)
        : template_net(model_path(dir, precision, "nanotrack_backbone_template", false)),
          search_net(model_path(dir, precision, "nanotrack_backbone", false)),
          head(model_path(dir, precision, "nanotrack_head", true)), use_rga(rga)
    {
        if (template_net.inputs.size() != 1 || template_net.outputs.size() != 1 ||
            search_net.inputs.size() != 1 || search_net.outputs.size() != 1 ||
            head.inputs.size() != 2 || head.outputs.size() != 2)
            throw std::runtime_error("Unexpected NanoTrack model I/O counts");
        Model::shape(template_net.inputs[0], 3, 127, 127);
        Model::shape(template_net.outputs[0], 96, 8, 8);
        Model::shape(search_net.inputs[0], 3, 255, 255);
        Model::shape(search_net.outputs[0], 96, 16, 16);
        Model::shape(head.inputs[0], 96, 8, 8);
        Model::shape(head.inputs[1], 96, 16, 16);
        Model::shape(head.outputs[0], 2, 15, 15);
        Model::shape(head.outputs[1], 4, 15, 15);
        template_image = template_net.image(127);
        search_image = search_net.image(255);
        head.import(0, template_net.floats(0, RKNN_TENSOR_NHWC));
        head.import(1, search_net.floats(0, RKNN_TENSOR_NHWC));
        cls = head.floats(0, RKNN_TENSOR_NCHW);
        loc = head.floats(1, RKNN_TENSOR_NCHW);
        for (int y = 0, i = 0; y < 15; ++y) {
            for (int x = 0; x < 15; ++x, ++i) {
                window[i] = (0.5f - 0.5f * std::cos(2.f * float(CV_PI) * x / 14)) *
                            (0.5f - 0.5f * std::cos(2.f * float(CV_PI) * y / 14));
            }
        }
    }

    Result initialize(const cv::Mat& frame, const cv::Rect2d& roi, GstObject* owner)
    {
        center = {float(roi.x + (roi.width-1)*0.5), float(roi.y + (roi.height-1)*0.5)};
        size = {float(roi.width), float(roi.height)};
        average = cv::mean(frame);
        crop(frame, std::lround(padded_size(size.width, size.height)), 127,
             template_net, template_image, owner);
        template_net.run();
        return {roi, 0, true};
    }

    Result track(const cv::Mat& frame, GstObject* owner)
    {
        const float template_size = padded_size(size.width, size.height);
        const float scale = 127.f / template_size;
        crop(frame, std::lround(template_size * 255.f / 127.f), 255, search_net, search_image, owner);
        search_net.run();
        head.run();
        head.sync(cls, RKNN_MEMORY_SYNC_FROM_DEVICE);
        head.sync(loc, RKNN_MEMORY_SYNC_FROM_DEVICE);
        const auto* scores = static_cast<const float*>(cls->virt_addr);
        const auto* boxes = static_cast<const float*>(loc->virt_addr);
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
    void crop(const cv::Mat& frame, int original, int output, Model& model,
              rknn_tensor_mem* destination, GstObject* owner)
    {
        // Keep CPU padding/crop and the explicit RKNN/RGA strides from the demo.
        if (original <= 0 || original > 65536)
            throw std::runtime_error("Unsupported crop size");
        const int x = static_cast<int>(std::floor(center.x-original*0.5f));
        const int y = static_cast<int>(std::floor(center.y-original*0.5f));
        const int source_stride = (original+3)&~3;
        const int aligned = (output+15)&~15;
        const size_t source_bytes = size_t(source_stride)*original*3;
        if (source_bytes > INT32_MAX) throw std::runtime_error("Crop too large for RGA import");
        cv::Mat padded(original, source_stride, CV_8UC3, average);
        const cv::Rect valid = cv::Rect(x, y, original, original) & cv::Rect(0, 0, frame.cols, frame.rows);
        if (valid.empty()) throw std::runtime_error("Crop outside frame");
        frame(valid).copyTo(padded(cv::Rect(valid.x-x, valid.y-y, valid.width, valid.height)));
        const size_t bytes = size_t(aligned)*aligned*3;
        if (destination->size < bytes) throw std::runtime_error("Image buffer too small for stride");
        if (use_rga) {
            RgaBuffer source(importbuffer_virtualaddr(padded.data, static_cast<int>(source_bytes)));
            RgaBuffer target(importbuffer_fd(destination->fd, static_cast<int>(bytes)));
            if (source.handle && target.handle) {
                auto src = wrapbuffer_handle(source.handle, original, original, RK_FORMAT_BGR_888, source_stride, original);
                auto dst = wrapbuffer_handle(target.handle, output, output, RK_FORMAT_BGR_888, aligned, aligned);
                model.sync(destination, RKNN_MEMORY_SYNC_TO_DEVICE);
                const IM_STATUS status = imresize(src, dst);
                // RGA wrote the buffer: do not flush stale CPU cache over its data.
                if (status == IM_STATUS_SUCCESS) return;
                GST_WARNING_OBJECT(owner, "RGA resize failed: %s; using CPU until stop", imStrError(status));
            } else {
                GST_WARNING_OBJECT(owner, "RGA buffer import failed; using CPU until stop");
            }
            use_rga = false;
        }
        model.sync(destination, RKNN_MEMORY_SYNC_FROM_DEVICE);
        std::memset(destination->virt_addr, 0, bytes);
        cv::Mat resized(output, output, CV_8UC3, destination->virt_addr, size_t(aligned)*3);
        cv::resize(padded(cv::Rect(0, 0, original, original)), resized, resized.size(), 0, 0, cv::INTER_LINEAR);
        model.sync(destination, RKNN_MEMORY_SYNC_TO_DEVICE);
    }
    // Head must die before the backbones whose feature memory it imports.
    Model template_net, search_net, head;
    rknn_tensor_mem *template_image = nullptr, *search_image = nullptr, *cls = nullptr, *loc = nullptr;
    bool use_rga;
    cv::Point2f center;
    cv::Size2f size;
    cv::Scalar average;
    std::array<float, 225> window{};
};

struct State {
    // ponytail: per-instance lock covers inference; snapshot settings if setter latency matters.
    std::mutex mutex;
    std::string roi, models_dir, precision = "mixed", resize = "auto";
    bool enabled = false, started = false, reset = true;
    GstVideoInfo info{};
    std::unique_ptr<Tracker> tracker;
};
} // namespace

typedef struct _GstRknnNanoTrack {
    GstBaseTransform parent;
    State* state;
} GstRknnNanoTrack;
typedef struct _GstRknnNanoTrackClass { GstBaseTransformClass parent_class; } GstRknnNanoTrackClass;
G_DEFINE_TYPE(GstRknnNanoTrack, gst_rknn_nano_track, GST_TYPE_BASE_TRANSFORM)

enum { PROP_0, PROP_ENABLED, PROP_ROI, PROP_MODELS_DIR, PROP_PRECISION, PROP_RESIZE };
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=BGR,width=[10,16384],height=[10,16384],interlace-mode=progressive"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=BGR,width=[10,16384],height=[10,16384],interlace-mode=progressive"));

static void set_property(GObject* object, guint id, const GValue* value, GParamSpec* spec)
{
    auto* self = reinterpret_cast<GstRknnNanoTrack*>(object);
    try {
        auto& s = *self->state;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (id >= PROP_MODELS_DIR && id <= PROP_RESIZE && s.started) {
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
        case PROP_PRECISION: s.precision = text(); break;
        case PROP_RESIZE: s.resize = text(); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
        }
    } catch (const std::exception& e) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, ("Cannot set tracker property"), ("%s", e.what()));
    }
}
static void get_property(GObject* object, guint id, GValue* value, GParamSpec* spec)
{
    auto& s = *reinterpret_cast<GstRknnNanoTrack*>(object)->state;
    std::lock_guard<std::mutex> lock(s.mutex);
    switch (id) {
    case PROP_ENABLED: g_value_set_boolean(value, s.enabled); break;
    case PROP_ROI: g_value_set_string(value, s.roi.c_str()); break;
    case PROP_MODELS_DIR: g_value_set_string(value, s.models_dir.c_str()); break;
    case PROP_PRECISION: g_value_set_string(value, s.precision.c_str()); break;
    case PROP_RESIZE: g_value_set_string(value, s.resize.c_str()); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}
static gboolean start(GstBaseTransform* base)
{
    auto& s = *reinterpret_cast<GstRknnNanoTrack*>(base)->state;
    std::lock_guard<std::mutex> lock(s.mutex);
    if ((s.precision != "fp16" && s.precision != "mixed" && s.precision != "int8" && s.precision != "int8-mmse") ||
        (s.resize != "auto" && s.resize != "cpu")) {
        GST_ELEMENT_ERROR(base, RESOURCE, SETTINGS, ("Invalid precision or resize setting"), (nullptr));
        return FALSE;
    }
    s.reset = true;
    s.started = true;
    return TRUE;
}
static gboolean stop(GstBaseTransform* base)
{
    auto& s = *reinterpret_cast<GstRknnNanoTrack*>(base)->state;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.tracker.reset();
    s.reset = true;
    s.started = false;
    return TRUE;
}
static gboolean set_caps(GstBaseTransform* base, GstCaps* input, GstCaps*)
{
    auto& s = *reinterpret_cast<GstRknnNanoTrack*>(base)->state;
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
        auto& s = *reinterpret_cast<GstRknnNanoTrack*>(base)->state;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.reset = true;
        break;
    }
    default: break;
    }
    return GST_BASE_TRANSFORM_CLASS(gst_rknn_nano_track_parent_class)->sink_event(base, event);
}
static GstFlowReturn transform_ip(GstBaseTransform* base, GstBuffer* buffer)
{
    auto& s = *reinterpret_cast<GstRknnNanoTrack*>(base)->state;
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
        if (!s.tracker) s.tracker = std::make_unique<Tracker>(s.models_dir, s.precision, s.resize == "auto");
        Result result = s.reset ? s.tracker->initialize(image, roi, GST_OBJECT(base)) :
                                 s.tracker->track(image, GST_OBJECT(base));
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
    delete reinterpret_cast<GstRknnNanoTrack*>(object)->state;
    G_OBJECT_CLASS(gst_rknn_nano_track_parent_class)->finalize(object);
}
static void gst_rknn_nano_track_class_init(GstRknnNanoTrackClass* klass)
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
        g_param_spec_string("models-dir", "Models directory", "Directory containing NanoTrack RKNN models", "", ready));
    g_object_class_install_property(object, PROP_PRECISION,
        g_param_spec_string("precision", "Precision", "Model filename set: fp16, mixed, int8, int8-mmse", "mixed", ready));
    g_object_class_install_property(object, PROP_RESIZE,
        g_param_spec_string("resize", "Resize", "auto: RGA with CPU fallback; cpu: OpenCV resize", "auto", ready));
    gst_element_class_set_static_metadata(element, "RKNN NanoTrack", "Filter/Metadata/Video",
        "Tracks one ROI with NanoTrackV3 and attaches ROI metadata", "example");
    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);
    transform->start = GST_DEBUG_FUNCPTR(start);
    transform->stop = GST_DEBUG_FUNCPTR(stop);
    transform->set_caps = GST_DEBUG_FUNCPTR(set_caps);
    transform->sink_event = GST_DEBUG_FUNCPTR(sink_event);
    transform->transform_ip = GST_DEBUG_FUNCPTR(transform_ip);
}
static void gst_rknn_nano_track_init(GstRknnNanoTrack* self)
{
    self->state = new State;
    gst_video_info_init(&self->state->info);
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
}
static gboolean plugin_init(GstPlugin* plugin)
{
    GST_DEBUG_CATEGORY_INIT(nanotrack_debug, "rknnnanotrack", 0, "RKNN NanoTrack");
    return gst_element_register(plugin, "rknnnanotrack", GST_RANK_NONE, gst_rknn_nano_track_get_type());
}
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, rknnnanotrack,
    "NanoTrackV3 RKNN tracking metadata", plugin_init, "1.0", "LGPL", "rknnnanotrack", "https://example.com")
