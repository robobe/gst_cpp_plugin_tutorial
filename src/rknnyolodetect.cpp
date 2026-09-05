#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <im2d.h>
#include <rknn_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PACKAGE
#define PACKAGE "rknnyolodetect"
#endif

GST_DEBUG_CATEGORY_STATIC(gst_rknn_yolo_detect_debug);
#define GST_CAT_DEFAULT gst_rknn_yolo_detect_debug

namespace {

constexpr int kBranchCount = 3;
constexpr int kOutputsPerBranch = 3;
constexpr int kDflBins = 16;
constexpr int kClassCount = 80;
constexpr int kModelWidth = 640;
constexpr int kModelHeight = 640;

struct Box {
    float left;
    float top;
    float right;
    float bottom;
};

struct Detection {
    int class_id;
    float confidence;
    Box box;
};

struct Letterbox {
    float scale_x;
    float scale_y;
    int pad_x;
    int pad_y;
};

static void require_rknn(int result, const char* operation)
{
    if (result != RKNN_SUCC) {
        throw std::runtime_error(
            std::string(operation) + " failed with RKNN status " +
            std::to_string(result)
        );
    }
}

static std::vector<unsigned char> read_file(const char* path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("cannot open RKNN model");
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("RKNN model is empty");
    }
    std::vector<unsigned char> data(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        throw std::runtime_error("cannot read RKNN model");
    }
    return data;
}

static int8_t quantize(float value, const rknn_tensor_attr& attr)
{
    const float quantized = value / attr.scale + attr.zp;
    return static_cast<int8_t>(std::clamp(std::lround(quantized), -128L, 127L));
}

static float dequantize(int8_t value, const rknn_tensor_attr& attr)
{
    return (static_cast<int>(value) - attr.zp) * attr.scale;
}

static float intersection_over_union(const Box& left, const Box& right)
{
    const float intersection_width =
        std::max(0.0F, std::min(left.right, right.right) - std::max(left.left, right.left));
    const float intersection_height =
        std::max(0.0F, std::min(left.bottom, right.bottom) - std::max(left.top, right.top));
    const float intersection = intersection_width * intersection_height;
    const float left_area = (left.right - left.left) * (left.bottom - left.top);
    const float right_area = (right.right - right.left) * (right.bottom - right.top);
    const float union_area = left_area + right_area - intersection;
    return union_area > 0.0F ? intersection / union_area : 0.0F;
}

static std::array<float, 4> decode_dfl(
    const int8_t* tensor,
    int cell,
    int grid_size,
    const rknn_tensor_attr& attr)
{
    std::array<float, 4> distances{};
    for (int side = 0; side < 4; ++side) {
        std::array<float, kDflBins> logits{};
        float maximum = -INFINITY;
        for (int bin = 0; bin < kDflBins; ++bin) {
            const int channel = side * kDflBins + bin;
            logits[bin] = dequantize(tensor[channel * grid_size + cell], attr);
            maximum = std::max(maximum, logits[bin]);
        }
        float denominator = 0.0F;
        for (float& logit : logits) {
            logit = std::exp(logit - maximum);
            denominator += logit;
        }
        for (int bin = 0; bin < kDflBins; ++bin) {
            distances[side] += logits[bin] / denominator * bin;
        }
    }
    return distances;
}

class RknnRuntime {
public:
    explicit RknnRuntime(const char* model_path)
    {
        std::vector<unsigned char> model = read_file(model_path);
        require_rknn(
            rknn_init(&context_, model.data(), static_cast<uint32_t>(model.size()), 0, nullptr),
            "rknn_init"
        );
        try {
            validate_model();
            model_input_.resize(kModelWidth * kModelHeight * 3);
        } catch (...) {
            rknn_destroy(context_);
            context_ = 0;
            throw;
        }
    }

    ~RknnRuntime()
    {
        if (context_ != 0) {
            rknn_destroy(context_);
        }
    }

    RknnRuntime(const RknnRuntime&) = delete;
    RknnRuntime& operator=(const RknnRuntime&) = delete;

    const rknn_sdk_version& version() const { return version_; }

    Letterbox preprocess(
        const unsigned char* rgb,
        int width,
        int height,
        int row_stride)
    {
        if (width <= 0 || height <= 0 || row_stride < width * 3) {
            throw std::runtime_error("invalid mapped RGB frame dimensions or stride");
        }

        const bool use_mapped_frame = row_stride == width * 3 && width % 16 == 0;
        const int source_stride_pixels = use_mapped_frame ? width : ((width + 15) & ~15);
        void* source_data = const_cast<unsigned char*>(rgb);
        if (!use_mapped_frame) {
            source_rgb_.resize(static_cast<size_t>(source_stride_pixels) * height * 3);
            for (int y = 0; y < height; ++y) {
                std::memcpy(
                    source_rgb_.data() + static_cast<size_t>(y) * source_stride_pixels * 3,
                    rgb + static_cast<size_t>(y) * row_stride,
                    static_cast<size_t>(width) * 3
                );
            }
            source_data = source_rgb_.data();
        }

        const float scale = std::min(
            static_cast<float>(kModelWidth) / width,
            static_cast<float>(kModelHeight) / height
        );
        int resized_width = std::max(4, static_cast<int>(width * scale));
        int resized_height = std::max(2, static_cast<int>(height * scale));
        resized_width = std::min(kModelWidth, resized_width - resized_width % 4);
        resized_height = std::min(kModelHeight, resized_height - resized_height % 2);
        const int pad_x = (kModelWidth - resized_width) / 2;
        const int pad_y = (kModelHeight - resized_height) / 2;

        rga_buffer_t source = wrapbuffer_virtualaddr_t(
            source_data,
            width,
            height,
            source_stride_pixels,
            height,
            RK_FORMAT_RGB_888
        );
        rga_buffer_t destination = wrapbuffer_virtualaddr_t(
            model_input_.data(),
            kModelWidth,
            kModelHeight,
            kModelWidth,
            kModelHeight,
            RK_FORMAT_RGB_888
        );
        const im_rect whole = {0, 0, kModelWidth, kModelHeight};
        IM_STATUS status = imfill(destination, whole, 0x72727272);
        if (status <= 0) {
            throw std::runtime_error(std::string("RGA fill failed: ") + imStrError(status));
        }

        rga_buffer_t pattern{};
        const im_rect source_rect = {0, 0, width, height};
        const im_rect destination_rect = {pad_x, pad_y, resized_width, resized_height};
        const im_rect pattern_rect{};
        status = improcess(
            source,
            destination,
            pattern,
            source_rect,
            destination_rect,
            pattern_rect,
            0
        );
        if (status <= 0) {
            throw std::runtime_error(std::string("RGA resize failed: ") + imStrError(status));
        }

        return {
            static_cast<float>(resized_width) / width,
            static_cast<float>(resized_height) / height,
            pad_x,
            pad_y
        };
    }

    std::vector<Detection> infer(
        const Letterbox& letterbox,
        int source_width,
        int source_height,
        float confidence_threshold,
        float iou_threshold,
        double& inference_milliseconds,
        double& postprocess_milliseconds)
    {
        const auto inference_start = std::chrono::steady_clock::now();
        rknn_input input{};
        input.index = 0;
        input.buf = model_input_.data();
        input.size = static_cast<uint32_t>(model_input_.size());
        input.type = RKNN_TENSOR_UINT8;
        input.fmt = RKNN_TENSOR_NHWC;
        input.pass_through = 0;
        require_rknn(rknn_inputs_set(context_, 1, &input), "rknn_inputs_set");
        require_rknn(rknn_run(context_, nullptr), "rknn_run");

        std::array<rknn_output, kBranchCount * kOutputsPerBranch> outputs{};
        for (size_t index = 0; index < outputs.size(); ++index) {
            outputs[index].index = static_cast<uint32_t>(index);
            outputs[index].want_float = 0;
        }
        require_rknn(
            rknn_outputs_get(context_, static_cast<uint32_t>(outputs.size()), outputs.data(), nullptr),
            "rknn_outputs_get"
        );
        const auto postprocess_start = std::chrono::steady_clock::now();

        try {
            std::vector<Detection> detections = decode(
                outputs,
                letterbox,
                source_width,
                source_height,
                confidence_threshold,
                iou_threshold
            );
            const auto done = std::chrono::steady_clock::now();
            inference_milliseconds =
                std::chrono::duration<double, std::milli>(postprocess_start - inference_start).count();
            postprocess_milliseconds =
                std::chrono::duration<double, std::milli>(done - postprocess_start).count();
            rknn_outputs_release(
                context_,
                static_cast<uint32_t>(outputs.size()),
                outputs.data()
            );
            return detections;
        } catch (...) {
            rknn_outputs_release(
                context_,
                static_cast<uint32_t>(outputs.size()),
                outputs.data()
            );
            throw;
        }
    }

private:
    void validate_model()
    {
        require_rknn(
            rknn_query(context_, RKNN_QUERY_SDK_VERSION, &version_, sizeof(version_)),
            "RKNN_QUERY_SDK_VERSION"
        );

        rknn_input_output_num counts{};
        require_rknn(
            rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &counts, sizeof(counts)),
            "RKNN_QUERY_IN_OUT_NUM"
        );
        if (counts.n_input != 1 || counts.n_output != output_attrs_.size()) {
            throw std::runtime_error("expected one input and nine YOLOv8 outputs");
        }

        input_attr_.index = 0;
        require_rknn(
            rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attr_, sizeof(input_attr_)),
            "RKNN_QUERY_INPUT_ATTR"
        );
        const bool nchw = input_attr_.fmt == RKNN_TENSOR_NCHW &&
            input_attr_.dims[0] == 1 && input_attr_.dims[1] == 3 &&
            input_attr_.dims[2] == kModelHeight && input_attr_.dims[3] == kModelWidth;
        const bool nhwc = input_attr_.fmt == RKNN_TENSOR_NHWC &&
            input_attr_.dims[0] == 1 && input_attr_.dims[1] == kModelHeight &&
            input_attr_.dims[2] == kModelWidth && input_attr_.dims[3] == 3;
        if (input_attr_.n_dims != 4 || (!nchw && !nhwc)) {
            throw std::runtime_error(
                "expected 640x640 RGB RKNN input; got fmt=" +
                std::to_string(input_attr_.fmt) + " dims=[" +
                std::to_string(input_attr_.dims[0]) + "," +
                std::to_string(input_attr_.dims[1]) + "," +
                std::to_string(input_attr_.dims[2]) + "," +
                std::to_string(input_attr_.dims[3]) + "]"
            );
        }

        const std::array<int, kBranchCount> grids = {80, 40, 20};
        for (size_t index = 0; index < output_attrs_.size(); ++index) {
            output_attrs_[index].index = static_cast<uint32_t>(index);
            require_rknn(
                rknn_query(
                    context_,
                    RKNN_QUERY_OUTPUT_ATTR,
                    &output_attrs_[index],
                    sizeof(rknn_tensor_attr)
                ),
                "RKNN_QUERY_OUTPUT_ATTR"
            );
            const rknn_tensor_attr& attr = output_attrs_[index];
            const int branch = static_cast<int>(index) / kOutputsPerBranch;
            const int within_branch = static_cast<int>(index) % kOutputsPerBranch;
            const int expected_channels =
                within_branch == 0 ? 4 * kDflBins : (within_branch == 1 ? kClassCount : 1);
            if (attr.n_dims != 4 || attr.fmt != RKNN_TENSOR_NCHW ||
                attr.type != RKNN_TENSOR_INT8 ||
                attr.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
                attr.dims[0] != 1 || attr.dims[1] != static_cast<uint32_t>(expected_channels) ||
                attr.dims[2] != static_cast<uint32_t>(grids[branch]) ||
                attr.dims[3] != static_cast<uint32_t>(grids[branch]) || attr.scale <= 0.0F) {
                throw std::runtime_error("unexpected RK3566 YOLOv8 INT8 output tensor layout");
            }
        }
    }

    std::vector<Detection> decode(
        const std::array<rknn_output, kBranchCount * kOutputsPerBranch>& outputs,
        const Letterbox& letterbox,
        int source_width,
        int source_height,
        float confidence_threshold,
        float iou_threshold) const
    {
        std::vector<Detection> candidates;
        const std::array<int, kBranchCount> grids = {80, 40, 20};

        for (int branch = 0; branch < kBranchCount; ++branch) {
            const int box_index = branch * kOutputsPerBranch;
            const int score_index = box_index + 1;
            const int sum_index = box_index + 2;
            const auto* boxes = static_cast<const int8_t*>(outputs[box_index].buf);
            const auto* scores = static_cast<const int8_t*>(outputs[score_index].buf);
            const auto* score_sums = static_cast<const int8_t*>(outputs[sum_index].buf);
            const int grid = grids[branch];
            const int grid_size = grid * grid;
            const float stride = static_cast<float>(kModelWidth) / grid;
            const int8_t score_threshold = quantize(
                confidence_threshold,
                output_attrs_[score_index]
            );
            const int8_t sum_threshold = quantize(
                confidence_threshold,
                output_attrs_[sum_index]
            );

            for (int row = 0; row < grid; ++row) {
                for (int column = 0; column < grid; ++column) {
                    const int cell = row * grid + column;
                    if (score_sums[cell] < sum_threshold) {
                        continue;
                    }

                    int best_class = -1;
                    int8_t best_quantized_score = INT8_MIN;
                    for (int class_id = 0; class_id < kClassCount; ++class_id) {
                        const int8_t score = scores[class_id * grid_size + cell];
                        if (score > best_quantized_score) {
                            best_quantized_score = score;
                            best_class = class_id;
                        }
                    }
                    if (best_quantized_score < score_threshold) {
                        continue;
                    }

                    const std::array<float, 4> distance = decode_dfl(
                        boxes,
                        cell,
                        grid_size,
                        output_attrs_[box_index]
                    );
                    const float model_left = (column + 0.5F - distance[0]) * stride;
                    const float model_top = (row + 0.5F - distance[1]) * stride;
                    const float model_right = (column + 0.5F + distance[2]) * stride;
                    const float model_bottom = (row + 0.5F + distance[3]) * stride;
                    Box source_box = {
                        (model_left - letterbox.pad_x) / letterbox.scale_x,
                        (model_top - letterbox.pad_y) / letterbox.scale_y,
                        (model_right - letterbox.pad_x) / letterbox.scale_x,
                        (model_bottom - letterbox.pad_y) / letterbox.scale_y
                    };
                    source_box.left = std::clamp(source_box.left, 0.0F, static_cast<float>(source_width));
                    source_box.top = std::clamp(source_box.top, 0.0F, static_cast<float>(source_height));
                    source_box.right = std::clamp(source_box.right, 0.0F, static_cast<float>(source_width));
                    source_box.bottom = std::clamp(source_box.bottom, 0.0F, static_cast<float>(source_height));
                    if (source_box.right > source_box.left && source_box.bottom > source_box.top) {
                        candidates.push_back({
                            best_class,
                            dequantize(best_quantized_score, output_attrs_[score_index]),
                            source_box
                        });
                    }
                }
            }
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const Detection& left, const Detection& right) {
                return left.confidence > right.confidence;
            }
        );

        // ponytail: O(n^2) NMS is adequate after score filtering; replace only if measured.
        std::vector<Detection> detections;
        for (const Detection& candidate : candidates) {
            const bool suppressed = std::any_of(
                detections.begin(),
                detections.end(),
                [&](const Detection& kept) {
                    return candidate.class_id == kept.class_id &&
                        intersection_over_union(candidate.box, kept.box) > iou_threshold;
                }
            );
            if (!suppressed) {
                detections.push_back(candidate);
            }
        }
        return detections;
    }

    rknn_context context_ = 0;
    rknn_sdk_version version_{};
    rknn_tensor_attr input_attr_{};
    std::array<rknn_tensor_attr, kBranchCount * kOutputsPerBranch> output_attrs_{};
    std::vector<unsigned char> source_rgb_;
    std::vector<unsigned char> model_input_;
};

}  // namespace

typedef struct _GstRknnYoloDetect {
    GstBaseTransform parent;
    GstVideoInfo video_info;
    gchar* model_path;
    gdouble confidence_threshold;
    gdouble iou_threshold;
    RknnRuntime* runtime;
} GstRknnYoloDetect;

typedef struct _GstRknnYoloDetectClass {
    GstBaseTransformClass parent_class;
} GstRknnYoloDetectClass;

#define GST_TYPE_RKNN_YOLO_DETECT (gst_rknn_yolo_detect_get_type())
G_DEFINE_TYPE(GstRknnYoloDetect, gst_rknn_yolo_detect, GST_TYPE_BASE_TRANSFORM)

enum {
    PROP_0,
    PROP_MODEL_PATH,
    PROP_CONFIDENCE_THRESHOLD,
    PROP_IOU_THRESHOLD
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=RGB")
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw,format=RGB")
);

static void gst_rknn_yolo_detect_set_property(
    GObject* object,
    guint property_id,
    const GValue* value,
    GParamSpec* pspec)
{
    auto* self = reinterpret_cast<GstRknnYoloDetect*>(object);
    switch (property_id) {
    case PROP_MODEL_PATH:
        g_free(self->model_path);
        self->model_path = g_value_dup_string(value);
        break;
    case PROP_CONFIDENCE_THRESHOLD:
        self->confidence_threshold = g_value_get_double(value);
        break;
    case PROP_IOU_THRESHOLD:
        self->iou_threshold = g_value_get_double(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void gst_rknn_yolo_detect_get_property(
    GObject* object,
    guint property_id,
    GValue* value,
    GParamSpec* pspec)
{
    auto* self = reinterpret_cast<GstRknnYoloDetect*>(object);
    switch (property_id) {
    case PROP_MODEL_PATH:
        g_value_set_string(value, self->model_path);
        break;
    case PROP_CONFIDENCE_THRESHOLD:
        g_value_set_double(value, self->confidence_threshold);
        break;
    case PROP_IOU_THRESHOLD:
        g_value_set_double(value, self->iou_threshold);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static gboolean gst_rknn_yolo_detect_start(GstBaseTransform* base)
{
    auto* self = reinterpret_cast<GstRknnYoloDetect*>(base);
    if (self->model_path == nullptr || self->model_path[0] == '\0') {
        GST_ELEMENT_ERROR(base, RESOURCE, NOT_FOUND, ("model-path is required"), (nullptr));
        return FALSE;
    }
    try {
        self->runtime = new RknnRuntime(self->model_path);
        GST_INFO_OBJECT(
            base,
            "RKNN API=%s driver=%s",
            self->runtime->version().api_version,
            self->runtime->version().drv_version
        );
        return TRUE;
    } catch (const std::exception& error) {
        GST_ELEMENT_ERROR(
            base,
            RESOURCE,
            OPEN_READ,
            ("failed to load RKNN YOLO model"),
            ("%s", error.what())
        );
        return FALSE;
    }
}

static gboolean gst_rknn_yolo_detect_stop(GstBaseTransform* base)
{
    auto* self = reinterpret_cast<GstRknnYoloDetect*>(base);
    delete self->runtime;
    self->runtime = nullptr;
    return TRUE;
}

static gboolean gst_rknn_yolo_detect_set_caps(
    GstBaseTransform* base,
    GstCaps* input_caps,
    GstCaps* output_caps)
{
    (void)output_caps;
    auto* self = reinterpret_cast<GstRknnYoloDetect*>(base);
    return gst_video_info_from_caps(&self->video_info, input_caps);
}

static GstFlowReturn gst_rknn_yolo_detect_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
{
    auto* self = reinterpret_cast<GstRknnYoloDetect*>(base);
    if (self->runtime == nullptr) {
        GST_ELEMENT_ERROR(base, CORE, FAILED, ("RKNN runtime is not initialized"), (nullptr));
        return GST_FLOW_ERROR;
    }

    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &self->video_info, buffer, GST_MAP_READ)) {
        GST_ELEMENT_ERROR(base, RESOURCE, READ, ("failed to map RGB video frame"), (nullptr));
        return GST_FLOW_ERROR;
    }

    bool mapped = true;
    try {
        const auto preprocess_start = std::chrono::steady_clock::now();
        const int source_width = GST_VIDEO_FRAME_WIDTH(&frame);
        const int source_height = GST_VIDEO_FRAME_HEIGHT(&frame);
        const Letterbox letterbox = self->runtime->preprocess(
            static_cast<const unsigned char*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
            source_width,
            source_height,
            GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0)
        );
        gst_video_frame_unmap(&frame);
        mapped = false;
        const auto inference_start = std::chrono::steady_clock::now();

        double inference_milliseconds = 0.0;
        double postprocess_milliseconds = 0.0;
        const std::vector<Detection> detections = self->runtime->infer(
            letterbox,
            source_width,
            source_height,
            static_cast<float>(self->confidence_threshold),
            static_cast<float>(self->iou_threshold),
            inference_milliseconds,
            postprocess_milliseconds
        );
        const auto metadata_start = std::chrono::steady_clock::now();

        for (const Detection& detection : detections) {
            const int x = static_cast<int>(std::floor(detection.box.left));
            const int y = static_cast<int>(std::floor(detection.box.top));
            const int right = static_cast<int>(std::ceil(detection.box.right));
            const int bottom = static_cast<int>(std::ceil(detection.box.bottom));
            GstVideoRegionOfInterestMeta* roi =
                gst_buffer_add_video_region_of_interest_meta(
                    buffer,
                    "yolo-detection",
                    x,
                    y,
                    right - x,
                    bottom - y
                );
            if (roi == nullptr) {
                throw std::runtime_error("failed to attach ROI metadata");
            }
            gst_video_region_of_interest_meta_add_param(
                roi,
                gst_structure_new(
                    "yolo",
                    "class-id", G_TYPE_INT, detection.class_id,
                    "confidence", G_TYPE_DOUBLE, static_cast<double>(detection.confidence),
                    nullptr
                )
            );
        }

        const auto done = std::chrono::steady_clock::now();
        const auto milliseconds = [](auto begin, auto end) {
            return std::chrono::duration<double, std::milli>(end - begin).count();
        };
        GST_LOG_OBJECT(
            base,
            "detections=%zu rga=%.3fms rknn=%.3fms postprocess=%.3fms metadata=%.3fms",
            detections.size(),
            milliseconds(preprocess_start, inference_start),
            inference_milliseconds,
            postprocess_milliseconds,
            milliseconds(metadata_start, done)
        );
        return GST_FLOW_OK;
    } catch (const std::exception& error) {
        if (mapped) {
            gst_video_frame_unmap(&frame);
        }
        GST_ELEMENT_ERROR(
            base,
            STREAM,
            FAILED,
            ("RKNN YOLO frame processing failed"),
            ("%s", error.what())
        );
        return GST_FLOW_ERROR;
    }
}

static void gst_rknn_yolo_detect_finalize(GObject* object)
{
    auto* self = reinterpret_cast<GstRknnYoloDetect*>(object);
    delete self->runtime;
    g_free(self->model_path);
    G_OBJECT_CLASS(gst_rknn_yolo_detect_parent_class)->finalize(object);
}

static void gst_rknn_yolo_detect_class_init(GstRknnYoloDetectClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass* transform_class = GST_BASE_TRANSFORM_CLASS(klass);

    object_class->set_property = gst_rknn_yolo_detect_set_property;
    object_class->get_property = gst_rknn_yolo_detect_get_property;
    object_class->finalize = gst_rknn_yolo_detect_finalize;

    const GParamFlags flags = static_cast<GParamFlags>(
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
    );
    g_object_class_install_property(
        object_class,
        PROP_MODEL_PATH,
        g_param_spec_string(
            "model-path",
            "Model path",
            "Path to the RK3566 YOLOv8 RKNN model",
            nullptr,
            flags
        )
    );
    g_object_class_install_property(
        object_class,
        PROP_CONFIDENCE_THRESHOLD,
        g_param_spec_double(
            "confidence-threshold",
            "Confidence threshold",
            "Minimum class confidence",
            0.0,
            1.0,
            0.25,
            flags
        )
    );
    g_object_class_install_property(
        object_class,
        PROP_IOU_THRESHOLD,
        g_param_spec_double(
            "iou-threshold",
            "IoU threshold",
            "Maximum same-class overlap retained by NMS",
            0.0,
            1.0,
            0.45,
            flags
        )
    );

    gst_element_class_set_static_metadata(
        element_class,
        "RKNN YOLO Detect",
        "Filter/Metadata/Video",
        "Runs synchronous RK3566 YOLOv8 inference and attaches ROI metadata",
        "example"
    );
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    transform_class->start = GST_DEBUG_FUNCPTR(gst_rknn_yolo_detect_start);
    transform_class->stop = GST_DEBUG_FUNCPTR(gst_rknn_yolo_detect_stop);
    transform_class->set_caps = GST_DEBUG_FUNCPTR(gst_rknn_yolo_detect_set_caps);
    transform_class->transform_ip = GST_DEBUG_FUNCPTR(gst_rknn_yolo_detect_transform_ip);
}

static void gst_rknn_yolo_detect_init(GstRknnYoloDetect* self)
{
    gst_video_info_init(&self->video_info);
    self->model_path = nullptr;
    self->confidence_threshold = 0.25;
    self->iou_threshold = 0.45;
    self->runtime = nullptr;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
}

static gboolean plugin_init(GstPlugin* plugin)
{
    GST_DEBUG_CATEGORY_INIT(
        gst_rknn_yolo_detect_debug,
        "rknnyolodetect",
        0,
        "RKNN YOLOv8 detection"
    );
    return gst_element_register(
        plugin,
        "rknnyolodetect",
        GST_RANK_NONE,
        GST_TYPE_RKNN_YOLO_DETECT
    );
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rknnyolodetect,
    "Synchronous RKNN YOLOv8 detection plugin",
    plugin_init,
    "1.0",
    "LGPL",
    "rknnyolodetect",
    "https://example.com"
)
