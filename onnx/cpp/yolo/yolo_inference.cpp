/*
 * YOLOv8 ONNX Runtime pipeline for assets/bus.jpg
 * ------------------------------------------------
 *
 * Each function below owns one transformation in the inference pipeline:
 *
 *   command-line paths
 *       -> load ONNX model and inspect tensor names/shapes
 *       -> load the source image as OpenCV BGR pixels
 *       -> letterbox resize while recording scale and padding
 *       -> BGR/HWC/uint8 to RGB/NCHW/FP32 normalized tensor
 *       -> synchronous ONNX Runtime inference
 *       -> decode raw [1, 4 + classes, candidates] YOLOv8 output
 *       -> confidence filter, source-coordinate restore, class-aware NMS
 *       -> print final bounding boxes in source-image pixels
 *
 * The bundled model expects [1, 3, 640, 640]. For its COCO export, output
 * [1, 84, 8400] means four center-based box values plus 80 class scores for
 * each of 8400 candidates. It was exported with nms=False, so ONNX Runtime
 * executes only the neural network; this program owns all pre/postprocessing.
 *
 * OpenCV DNN is not used. OpenCV only loads and resizes the image. Run without
 * arguments for the repository model and bus image, or pass <model> <image>.
 */

#include <onnxruntime_cxx_api.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef YOLO_MODEL_PATH
#define YOLO_MODEL_PATH "models/yolov8n.onnx"
#endif

#ifndef YOLO_IMAGE_PATH
#define YOLO_IMAGE_PATH "assets/bus.jpg"
#endif

struct Arguments {
    std::string model_path;
    std::string image_path;
};

struct ModelContract {
    std::string input_name;
    std::string output_name;
    std::vector<int64_t> input_shape;
    int input_width;
    int input_height;
};

struct Letterbox {
    cv::Mat image;
    float scale;
    int pad_x;
    int pad_y;
};

struct InferenceOutput {
    std::vector<float> values;
    int64_t channels;
    int64_t candidate_count;
};

struct Detection {
    int class_id;
    float confidence;
    cv::Rect box;
};

static Arguments parse_arguments(int argc, char* argv[])
{
    // Stage 1: Select the default assets or the two paths supplied by the user.
    if (argc != 1 && argc != 3) {
        throw std::runtime_error(
            std::string("usage: ") + argv[0] + " [<yolov8.onnx> <image>]"
        );
    }
    return argc == 3
        ? Arguments{argv[1], argv[2]}
        : Arguments{YOLO_MODEL_PATH, YOLO_IMAGE_PATH};
}

static Ort::Session create_session(Ort::Env& environment, const std::string& model_path)
{
    // Stage 2: Load and optimize the graph. No provider is added, so CPU is used.
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return Ort::Session(environment, model_path.c_str(), options);
}

static ModelContract inspect_model(Ort::Session& session)
{
    // Stage 3: Fetch names and dimensions instead of hard-coding graph node names.
    if (session.GetInputCount() != 1 || session.GetOutputCount() != 1) {
        throw std::runtime_error("expected exactly one model input and one output");
    }

    Ort::AllocatorWithDefaultOptions allocator;
    const auto allocated_input_name = session.GetInputNameAllocated(0, allocator);
    const auto allocated_output_name = session.GetOutputNameAllocated(0, allocator);

    const Ort::TypeInfo input_type = session.GetInputTypeInfo(0);
    const auto input_info = input_type.GetTensorTypeAndShapeInfo();
    const std::vector<int64_t> input_shape = input_info.GetShape();
    if (input_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        input_shape.size() != 4 || input_shape[0] != 1 || input_shape[1] != 3 ||
        input_shape[2] <= 0 || input_shape[3] <= 0) {
        throw std::runtime_error("expected fixed FP32 input [1, 3, height, width]");
    }

    return {
        allocated_input_name.get(),
        allocated_output_name.get(),
        input_shape,
        static_cast<int>(input_shape[3]),
        static_cast<int>(input_shape[2])
    };
}

static cv::Mat load_image(const std::string& image_path)
{
    // Stage 4: Decode the image. cv::imread returns interleaved uint8 BGR pixels.
    cv::Mat image = cv::imread(image_path);
    if (image.empty()) {
        throw std::runtime_error("cannot read image: " + image_path);
    }
    return image;
}

static Letterbox letterbox_image(
    const cv::Mat& image,
    int input_width,
    int input_height)
{
    // Stage 5: Preserve aspect ratio and center the resized image on gray padding.
    const float scale = std::min(
        static_cast<float>(input_width) / image.cols,
        static_cast<float>(input_height) / image.rows
    );
    const int resized_width = std::lround(image.cols * scale);
    const int resized_height = std::lround(image.rows * scale);
    const int pad_x = (input_width - resized_width) / 2;
    const int pad_y = (input_height - resized_height) / 2;

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(resized_width, resized_height));
    cv::Mat canvas(input_height, input_width, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(canvas(cv::Rect(pad_x, pad_y, resized_width, resized_height)));
    return {std::move(canvas), scale, pad_x, pad_y};
}

static std::vector<float> create_input_tensor(const cv::Mat& letterboxed_image)
{
    // Stage 6: Convert BGR HWC bytes into normalized planar RGB float values.
    const size_t plane_size = letterboxed_image.total();
    std::vector<float> input(3 * plane_size);
    for (int y = 0; y < letterboxed_image.rows; ++y) {
        for (int x = 0; x < letterboxed_image.cols; ++x) {
            const cv::Vec3b bgr = letterboxed_image.at<cv::Vec3b>(y, x);
            const size_t index = static_cast<size_t>(y) * letterboxed_image.cols + x;
            input[index] = bgr[2] / 255.0F;
            input[plane_size + index] = bgr[1] / 255.0F;
            input[2 * plane_size + index] = bgr[0] / 255.0F;
        }
    }
    return input;
}

static InferenceOutput run_inference(
    Ort::Session& session,
    const ModelContract& model,
    std::vector<float>& input_data)
{
    // Stage 7: Wrap application memory as a tensor and run synchronously.
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator,
        OrtMemTypeDefault
    );
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory,
        input_data.data(),
        input_data.size(),
        model.input_shape.data(),
        model.input_shape.size()
    );

    const char* input_names[] = {model.input_name.c_str()};
    const char* output_names[] = {model.output_name.c_str()};
    std::vector<Ort::Value> outputs = session.Run(
        Ort::RunOptions{nullptr},
        input_names,
        &input_tensor,
        1,
        output_names,
        1
    );

    const auto output_info = outputs[0].GetTensorTypeAndShapeInfo();
    const std::vector<int64_t> shape = output_info.GetShape();
    if (output_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        shape.size() != 3 || shape[0] != 1 || shape[1] < 5 || shape[2] <= 0) {
        throw std::runtime_error(
            "expected raw FP32 YOLOv8 output [1, 4 + classes, candidates]"
        );
    }

    const float* data = outputs[0].GetTensorData<float>();
    const size_t value_count = static_cast<size_t>(shape[1] * shape[2]);
    return {{data, data + value_count}, shape[1], shape[2]};
}

static float intersection_over_union(const cv::Rect& left, const cv::Rect& right)
{
    const int intersection = (left & right).area();
    const int union_area = left.area() + right.area() - intersection;
    return union_area > 0
        ? static_cast<float>(intersection) / union_area
        : 0.0F;
}

static cv::Rect restore_source_box(
    float center_x,
    float center_y,
    float width,
    float height,
    const Letterbox& letterbox,
    const cv::Size& source_size)
{
    // Stage 8a: Undo letterbox padding/scale and clip the box to the source image.
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
    const InferenceOutput& output,
    const Letterbox& letterbox,
    const cv::Size& source_size,
    float confidence_threshold,
    float iou_threshold)
{
    // Stage 8b: Select each candidate's strongest class and filter weak boxes.
    const int class_count = static_cast<int>(output.channels - 4);
    std::vector<Detection> candidates;
    for (int64_t candidate = 0; candidate < output.candidate_count; ++candidate) {
        int best_class = 0;
        float best_score = output.values[4 * output.candidate_count + candidate];
        for (int class_id = 1; class_id < class_count; ++class_id) {
            const float score =
                output.values[(4 + class_id) * output.candidate_count + candidate];
            if (score > best_score) {
                best_score = score;
                best_class = class_id;
            }
        }
        if (best_score < confidence_threshold) {
            continue;
        }

        const cv::Rect box = restore_source_box(
            output.values[candidate],
            output.values[output.candidate_count + candidate],
            output.values[2 * output.candidate_count + candidate],
            output.values[3 * output.candidate_count + candidate],
            letterbox,
            source_size
        );
        if (box.area() > 0) {
            candidates.push_back({best_class, best_score, box});
        }
    }

    // Stage 8c: Keep strong boxes and suppress overlaps only within the same class.
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const Detection& left, const Detection& right) {
            return left.confidence > right.confidence;
        }
    );
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

static void print_detections(const std::vector<Detection>& detections)
{
    // Stage 9: Report source-image coordinates and numeric COCO class IDs.
    std::cout << "detections: " << detections.size() << '\n';
    for (const Detection& detection : detections) {
        std::cout << "class=" << detection.class_id
                  << " confidence=" << detection.confidence
                  << " x=" << detection.box.x
                  << " y=" << detection.box.y
                  << " width=" << detection.box.width
                  << " height=" << detection.box.height << '\n';
    }
}

static void run_pipeline(const Arguments& arguments)
{
    // This function connects the independent stages without hiding their order.
    Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "yolov8-cpp");
    Ort::Session session = create_session(environment, arguments.model_path);
    const ModelContract model = inspect_model(session);
    const cv::Mat image = load_image(arguments.image_path);
    const Letterbox letterbox = letterbox_image(
        image,
        model.input_width,
        model.input_height
    );
    std::vector<float> input = create_input_tensor(letterbox.image);
    const InferenceOutput output = run_inference(session, model, input);
    const std::vector<Detection> detections = postprocess(
        output,
        letterbox,
        image.size(),
        0.25F,
        0.45F
    );

    std::cout << "input:  " << model.input_name << " [1, 3, "
              << model.input_height << ", " << model.input_width << "]\n";
    std::cout << "output: " << model.output_name << " [1, "
              << output.channels << ", " << output.candidate_count << "]\n";
    print_detections(detections);
}

int main(int argc, char* argv[])
try {
    run_pipeline(parse_arguments(argc, argv));
    return 0;
} catch (const Ort::Exception& error) {
    std::cerr << "ONNX Runtime error: " << error.what() << '\n';
    return 1;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
