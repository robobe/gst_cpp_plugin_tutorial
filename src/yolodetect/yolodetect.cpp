#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>

#include <onnxruntime_cxx_api.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PACKAGE
#define PACKAGE "yolodetect"
#endif

GST_DEBUG_CATEGORY_STATIC(gst_yolo_detect_debug);
#define GST_CAT_DEFAULT gst_yolo_detect_debug

struct Detection {
    int class_id;
    float confidence;
    cv::Rect box;
};

struct Letterbox {
    std::vector<float> input;
    float scale;
    int pad_x;
    int pad_y;
};

struct YoloRuntime {
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "yolodetect"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::vector<int64_t> input_shape;
    std::string input_name;
    std::string output_name;
    int input_width = 0;
    int input_height = 0;
    int64_t output_channels = 0;
    int64_t candidate_count = 0;

    explicit YoloRuntime(const char* model_path, int intra_op_threads)
    {
        if (intra_op_threads > 0) {
            options.SetIntraOpNumThreads(intra_op_threads);
        }
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session = std::make_unique<Ort::Session>(environment, model_path, options);

        if (session->GetInputCount() != 1 || session->GetOutputCount() != 1) {
            throw std::runtime_error("expected exactly one model input and one output");
        }

        const Ort::TypeInfo input_type = session->GetInputTypeInfo(0);
        const auto input_info = input_type.GetTensorTypeAndShapeInfo();
        input_shape = input_info.GetShape();
        if (input_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            input_shape.size() != 4 || input_shape[0] != 1 || input_shape[1] != 3 ||
            input_shape[2] <= 0 || input_shape[3] <= 0) {
            throw std::runtime_error("expected static FP32 input [1, 3, height, width]");
        }
        input_height = static_cast<int>(input_shape[2]);
        input_width = static_cast<int>(input_shape[3]);

        const Ort::TypeInfo output_type = session->GetOutputTypeInfo(0);
        const auto output_info = output_type.GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> output_shape = output_info.GetShape();
        if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            output_shape.size() != 3 || output_shape[0] != 1 ||
            output_shape[1] < 5 || output_shape[2] <= 0) {
            throw std::runtime_error(
                "expected raw FP32 YOLO output [1, 4 + classes, candidates]"
            );
        }
        output_channels = output_shape[1];
        candidate_count = output_shape[2];

        Ort::AllocatorWithDefaultOptions allocator;
        const auto allocated_input_name = session->GetInputNameAllocated(0, allocator);
        const auto allocated_output_name = session->GetOutputNameAllocated(0, allocator);
        input_name = allocated_input_name.get();
        output_name = allocated_output_name.get();
    }
};

typedef struct _GstYoloDetect {
    GstBaseTransform parent;
    GstVideoInfo video_info;
    gchar* model_path;
    gint intra_op_threads;
    gdouble confidence_threshold;
    gdouble iou_threshold;
    YoloRuntime* runtime;
} GstYoloDetect;

typedef struct _GstYoloDetectClass {
    GstBaseTransformClass parent_class;
} GstYoloDetectClass;

#define GST_TYPE_YOLO_DETECT (gst_yolo_detect_get_type())

G_DEFINE_TYPE(GstYoloDetect, gst_yolo_detect, GST_TYPE_BASE_TRANSFORM)

enum {
    PROP_0,
    PROP_MODEL_PATH,
    PROP_INTRA_OP_THREADS,
    PROP_CONFIDENCE_THRESHOLD,
    PROP_IOU_THRESHOLD
};

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE(
        "sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-raw,format=RGB")
    );

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-raw,format=RGB")
    );

static Letterbox preprocess(const cv::Mat& rgb, int input_width, int input_height)
{
    const float scale = std::min(
        static_cast<float>(input_width) / rgb.cols,
        static_cast<float>(input_height) / rgb.rows
    );
    const int resized_width = std::lround(rgb.cols * scale);
    const int resized_height = std::lround(rgb.rows * scale);
    const int pad_x = (input_width - resized_width) / 2;
    const int pad_y = (input_height - resized_height) / 2;

    cv::Mat resized;
    cv::resize(rgb, resized, cv::Size(resized_width, resized_height));
    cv::Mat padded(input_height, input_width, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(padded(cv::Rect(pad_x, pad_y, resized_width, resized_height)));

    const size_t plane_size = static_cast<size_t>(input_width) * input_height;
    std::vector<float> input(3 * plane_size);
    for (int y = 0; y < input_height; ++y) {
        for (int x = 0; x < input_width; ++x) {
            const cv::Vec3b pixel = padded.at<cv::Vec3b>(y, x);
            const size_t index = static_cast<size_t>(y) * input_width + x;
            input[index] = pixel[0] / 255.0F;
            input[plane_size + index] = pixel[1] / 255.0F;
            input[2 * plane_size + index] = pixel[2] / 255.0F;
        }
    }
    return {std::move(input), scale, pad_x, pad_y};
}

static float intersection_over_union(const cv::Rect& left, const cv::Rect& right)
{
    const int intersection = (left & right).area();
    return static_cast<float>(intersection) /
        static_cast<float>(left.area() + right.area() - intersection);
}

static cv::Rect to_source_box(
    float center_x,
    float center_y,
    float width,
    float height,
    const Letterbox& letterbox,
    const cv::Size& source_size)
{
    float left = (center_x - width / 2.0F - letterbox.pad_x) / letterbox.scale;
    float top = (center_y - height / 2.0F - letterbox.pad_y) / letterbox.scale;
    float right = (center_x + width / 2.0F - letterbox.pad_x) / letterbox.scale;
    float bottom = (center_y + height / 2.0F - letterbox.pad_y) / letterbox.scale;

    left = std::clamp(left, 0.0F, static_cast<float>(source_size.width));
    top = std::clamp(top, 0.0F, static_cast<float>(source_size.height));
    right = std::clamp(right, 0.0F, static_cast<float>(source_size.width));
    bottom = std::clamp(bottom, 0.0F, static_cast<float>(source_size.height));

    const int x = static_cast<int>(std::floor(left));
    const int y = static_cast<int>(std::floor(top));
    return {
        x,
        y,
        static_cast<int>(std::ceil(right)) - x,
        static_cast<int>(std::ceil(bottom)) - y
    };
}

static std::vector<Detection> postprocess(
    const float* output,
    int64_t channels,
    int64_t candidate_count,
    const Letterbox& letterbox,
    const cv::Size& source_size,
    float confidence_threshold,
    float iou_threshold)
{
    const int class_count = static_cast<int>(channels - 4);
    std::vector<Detection> candidates;

    for (int64_t candidate = 0; candidate < candidate_count; ++candidate) {
        int best_class = 0;
        float best_score = output[4 * candidate_count + candidate];
        for (int class_id = 1; class_id < class_count; ++class_id) {
            const float score = output[(4 + class_id) * candidate_count + candidate];
            if (score > best_score) {
                best_score = score;
                best_class = class_id;
            }
        }
        if (best_score < confidence_threshold) {
            continue;
        }

        const cv::Rect box = to_source_box(
            output[candidate],
            output[candidate_count + candidate],
            output[2 * candidate_count + candidate],
            output[3 * candidate_count + candidate],
            letterbox,
            source_size
        );
        if (box.area() > 0) {
            candidates.push_back({best_class, best_score, box});
        }
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Detection& left, const Detection& right) {
            return left.confidence > right.confidence;
        }
    );

    // ponytail: O(n^2) NMS is enough for YOLOv8n; replace it if profiling says otherwise.
    std::vector<Detection> detections;
    for (const Detection& candidate : candidates) {
        const bool overlaps = std::any_of(
            detections.begin(),
            detections.end(),
            [&](const Detection& kept) {
                return candidate.class_id == kept.class_id &&
                    intersection_over_union(candidate.box, kept.box) > iou_threshold;
            }
        );
        if (!overlaps) {
            detections.push_back(candidate);
        }
    }
    return detections;
}

static void gst_yolo_detect_set_property(
    GObject* object,
    guint property_id,
    const GValue* value,
    GParamSpec* pspec)
{
    GstYoloDetect* self = reinterpret_cast<GstYoloDetect*>(object);
    switch (property_id) {
    case PROP_MODEL_PATH:
        g_free(self->model_path);
        self->model_path = g_value_dup_string(value);
        break;
    case PROP_INTRA_OP_THREADS:
        self->intra_op_threads = g_value_get_int(value);
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

static void gst_yolo_detect_get_property(
    GObject* object,
    guint property_id,
    GValue* value,
    GParamSpec* pspec)
{
    GstYoloDetect* self = reinterpret_cast<GstYoloDetect*>(object);
    switch (property_id) {
    case PROP_MODEL_PATH:
        g_value_set_string(value, self->model_path);
        break;
    case PROP_INTRA_OP_THREADS:
        g_value_set_int(value, self->intra_op_threads);
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

static gboolean gst_yolo_detect_start(GstBaseTransform* base)
{
    GstYoloDetect* self = reinterpret_cast<GstYoloDetect*>(base);
    if (self->model_path == nullptr || self->model_path[0] == '\0') {
        GST_ELEMENT_ERROR(base, RESOURCE, NOT_FOUND, ("model-path is required"), (nullptr));
        return FALSE;
    }

    try {
        self->runtime = new YoloRuntime(self->model_path, self->intra_op_threads);
        return TRUE;
    } catch (const std::exception& error) {
        GST_ELEMENT_ERROR(
            base,
            RESOURCE,
            OPEN_READ,
            ("failed to load YOLO model"),
            ("%s", error.what())
        );
        return FALSE;
    }
}

static gboolean gst_yolo_detect_stop(GstBaseTransform* base)
{
    GstYoloDetect* self = reinterpret_cast<GstYoloDetect*>(base);
    delete self->runtime;
    self->runtime = nullptr;
    return TRUE;
}

static gboolean gst_yolo_detect_set_caps(
    GstBaseTransform* base,
    GstCaps* input_caps,
    GstCaps* output_caps)
{
    (void)output_caps;
    GstYoloDetect* self = reinterpret_cast<GstYoloDetect*>(base);
    return gst_video_info_from_caps(&self->video_info, input_caps);
}

static GstFlowReturn gst_yolo_detect_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
{
    GstYoloDetect* self = reinterpret_cast<GstYoloDetect*>(base);
    if (self->runtime == nullptr) {
        GST_ELEMENT_ERROR(base, CORE, FAILED, ("YOLO runtime is not initialized"), (nullptr));
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
        const cv::Mat rgb(
            GST_VIDEO_FRAME_HEIGHT(&frame),
            GST_VIDEO_FRAME_WIDTH(&frame),
            CV_8UC3,
            GST_VIDEO_FRAME_PLANE_DATA(&frame, 0),
            GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0)
        );
        Letterbox letterbox = preprocess(
            rgb,
            self->runtime->input_width,
            self->runtime->input_height
        );
        const cv::Size source_size(rgb.cols, rgb.rows);
        gst_video_frame_unmap(&frame);
        mapped = false;
        const auto inference_start = std::chrono::steady_clock::now();

        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator,
            OrtMemTypeDefault
        );
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memory,
            letterbox.input.data(),
            letterbox.input.size(),
            self->runtime->input_shape.data(),
            self->runtime->input_shape.size()
        );
        const char* input_names[] = {self->runtime->input_name.c_str()};
        const char* output_names[] = {self->runtime->output_name.c_str()};
        std::vector<Ort::Value> outputs = self->runtime->session->Run(
            Ort::RunOptions{nullptr},
            input_names,
            &input,
            1,
            output_names,
            1
        );
        const auto postprocess_start = std::chrono::steady_clock::now();

        const std::vector<Detection> detections = postprocess(
            outputs[0].GetTensorData<float>(),
            self->runtime->output_channels,
            self->runtime->candidate_count,
            letterbox,
            source_size,
            static_cast<float>(self->confidence_threshold),
            static_cast<float>(self->iou_threshold)
        );

        for (size_t index = 0; index < detections.size(); ++index) {
            const Detection& detection = detections[index];
            GstVideoRegionOfInterestMeta* roi =
                gst_buffer_add_video_region_of_interest_meta(
                    buffer,
                    "yolo-detection",
                    detection.box.x,
                    detection.box.y,
                    detection.box.width,
                    detection.box.height
                );
            if (roi == nullptr) {
                throw std::runtime_error("failed to attach ROI metadata");
            }
            roi->id = static_cast<gint>(index);
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
        const auto milliseconds = [](auto start, auto end) {
            return std::chrono::duration<double, std::milli>(end - start).count();
        };
        GST_LOG_OBJECT(
            base,
            "detections=%zu preprocess=%.3fms inference=%.3fms postprocess=%.3fms",
            detections.size(),
            milliseconds(preprocess_start, inference_start),
            milliseconds(inference_start, postprocess_start),
            milliseconds(postprocess_start, done)
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
            ("YOLO frame processing failed"),
            ("%s", error.what())
        );
        return GST_FLOW_ERROR;
    }
}

static void gst_yolo_detect_finalize(GObject* object)
{
    GstYoloDetect* self = reinterpret_cast<GstYoloDetect*>(object);
    delete self->runtime;
    g_free(self->model_path);
    G_OBJECT_CLASS(gst_yolo_detect_parent_class)->finalize(object);
}

static void gst_yolo_detect_class_init(GstYoloDetectClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass* transform_class = GST_BASE_TRANSFORM_CLASS(klass);

    object_class->set_property = gst_yolo_detect_set_property;
    object_class->get_property = gst_yolo_detect_get_property;
    object_class->finalize = gst_yolo_detect_finalize;

    const GParamFlags property_flags = static_cast<GParamFlags>(
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY
    );
    g_object_class_install_property(
        object_class,
        PROP_MODEL_PATH,
        g_param_spec_string(
            "model-path",
            "Model path",
            "Path to a raw YOLO detection ONNX model",
            nullptr,
            property_flags
        )
    );
    g_object_class_install_property(
        object_class,
        PROP_INTRA_OP_THREADS,
        g_param_spec_int(
            "intra-op-threads",
            "Intra-op threads",
            "ONNX Runtime inference threads; 0 uses ONNX Runtime's default",
            0,
            G_MAXINT,
            0,
            property_flags
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
            property_flags
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
            property_flags
        )
    );

    gst_element_class_set_static_metadata(
        element_class,
        "YOLO Detect",
        "Filter/Metadata/Video",
        "Runs synchronous YOLO inference and attaches ROI metadata",
        "example"
    );
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    transform_class->start = GST_DEBUG_FUNCPTR(gst_yolo_detect_start);
    transform_class->stop = GST_DEBUG_FUNCPTR(gst_yolo_detect_stop);
    transform_class->set_caps = GST_DEBUG_FUNCPTR(gst_yolo_detect_set_caps);
    transform_class->transform_ip = GST_DEBUG_FUNCPTR(gst_yolo_detect_transform_ip);
}

static void gst_yolo_detect_init(GstYoloDetect* self)
{
    gst_video_info_init(&self->video_info);
    self->model_path = nullptr;
    self->intra_op_threads = 0;
    self->confidence_threshold = 0.25;
    self->iou_threshold = 0.45;
    self->runtime = nullptr;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
}

static gboolean plugin_init(GstPlugin* plugin)
{
    GST_DEBUG_CATEGORY_INIT(
        gst_yolo_detect_debug,
        "yolodetect",
        0,
        "YOLO detection"
    );
    return gst_element_register(plugin, "yolodetect", GST_RANK_NONE, GST_TYPE_YOLO_DETECT);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    yolodetect,
    "Synchronous YOLO ONNX Runtime detector",
    plugin_init,
    "1.0",
    "LGPL",
    "yolodetect",
    "https://example.com"
)
