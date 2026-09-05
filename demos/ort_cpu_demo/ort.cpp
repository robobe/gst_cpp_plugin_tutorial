/*
 * YOLOv8 + ONNX Runtime pipeline
 * =================================
 *
 *   image file
 *   BGR, original size
 *         |
 *         v
 *   +------------------------+
 *   | Letterbox preprocessing|
 *   | - keep aspect ratio    |
 *   | - resize               |
 *   | - pad with value 114   |
 *   +------------------------+
 *         |
 *         v
 *   +------------------------+
 *   | Tensor conversion      |
 *   | BGR -> RGB             |
 *   | uint8 -> float [0, 1]  |
 *   | HWC -> NCHW            |
 *   +------------------------+
 *         |
 *         v
 *   input tensor [1, 3, H, W]
 *         |
 *         v
 *   +------------------------+
 *   | ONNX Runtime           |
 *   | Ort::Session::Run()    |
 *   +------------------------+
 *         |
 *         v
 *   output tensor [1, 4 + classes, candidates]
 *         |
 *         v
 *   +------------------------+
 *   | YOLOv8 postprocessing  |
 *   | - decode cx, cy, w, h  |
 *   | - choose best class    |
 *   | - confidence filter    |
 *   | - undo padding/resize  |
 *   | - clip to image        |
 *   | - class-aware NMS      |
 *   +------------------------+
 *         |
 *         v
 *   class, confidence, x, y, width, height
 *
 * Preprocessing records scale and padding so boxes predicted in the model's
 * letterboxed coordinate system can be mapped back to the original image.
 *
 * A raw YOLOv8 detection export (nms=False) stores one candidate per output
 * column. Its first four rows are center_x, center_y, width, and height. The
 * remaining rows are class scores. The highest class score is the candidate's
 * confidence; YOLOv8 has no separate objectness row in this output format.
 *
 * NMS (non-maximum suppression) keeps the strongest box and removes weaker,
 * highly overlapping boxes of the same class. ONNX Runtime only executes the
 * model; this file performs preprocessing and postprocessing explicitly.
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

static Letterbox preprocess(const cv::Mat& image, int input_width, int input_height)
{
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

    cv::Mat padded(input_height, input_width, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(padded(cv::Rect(pad_x, pad_y, resized_width, resized_height)));

    const size_t plane_size = static_cast<size_t>(input_width) * input_height;
    std::vector<float> input(3 * plane_size);
    for (int y = 0; y < input_height; ++y) {
        for (int x = 0; x < input_width; ++x) {
            const cv::Vec3b bgr = padded.at<cv::Vec3b>(y, x);
            const size_t index = static_cast<size_t>(y) * input_width + x;
            input[index] = bgr[2] / 255.0F;
            input[plane_size + index] = bgr[1] / 255.0F;
            input[2 * plane_size + index] = bgr[0] / 255.0F;
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
    const int box_width = static_cast<int>(std::ceil(right)) - x;
    const int box_height = static_cast<int>(std::ceil(bottom)) - y;
    return {x, y, box_width, box_height};
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

    // ponytail: O(n^2) NMS is enough for YOLOv8n; use an optimized NMS only if profiling requires it.
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

int main(int argc, char** argv)
try {
    // 1. Read the model and image paths from the command line.
    const std::string program_name = argv[0];
    if (argc != 3) {
        std::cerr << "usage: " << program_name << " <yolov8.onnx> <image>\n";
        return 2;
    }
    const std::string model_path = argv[1];
    const std::string image_path = argv[2];

    // 2. Load the source image. OpenCV stores color images in BGR order.
    const cv::Mat image = cv::imread(image_path);
    if (image.empty()) {
        throw std::runtime_error("cannot read image: " + image_path);
    }

    // 3. Create a CPU ONNX Runtime session and enable graph optimizations.
    Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "yolov8-demo");
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    Ort::Session session(environment, model_path.c_str(), options);

    // 4. This minimal demo supports models with one image input and one output.
    if (session.GetInputCount() != 1 || session.GetOutputCount() != 1) {
        throw std::runtime_error("expected exactly one model input and one output");
    }

    // 5. Inspect the model instead of hard-coding its input height and width.
    const std::vector<int64_t> input_shape = session.GetInputTypeInfo(0)
        .GetTensorTypeAndShapeInfo()
        .GetShape();
    constexpr size_t batch_axis = 0;
    constexpr size_t channel_axis = 1;
    constexpr size_t height_axis = 2;
    constexpr size_t width_axis = 3;
    if (input_shape.size() != 4 ||
        input_shape[batch_axis] != 1 ||
        input_shape[channel_axis] != 3 ||
        input_shape[height_axis] <= 0 ||
        input_shape[width_axis] <= 0) {
        throw std::runtime_error("expected a static float input shaped [1, 3, height, width]");
    }

    // 6. Letterbox the image and build the normalized RGB NCHW float buffer.
    const int input_height = static_cast<int>(input_shape[height_axis]);
    const int input_width = static_cast<int>(input_shape[width_axis]);
    Letterbox letterbox = preprocess(image, input_width, input_height);

    // 7. Wrap our float buffer as an ONNX Runtime tensor without copying it.
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator,
        OrtMemTypeDefault
    );
    Ort::Value input = Ort::Value::CreateTensor<float>(
        memory,
        letterbox.input.data(),
        letterbox.input.size(),
        input_shape.data(),
        input_shape.size()
    );

    // 8. Ask the model for its actual input/output node names.
    Ort::AllocatorWithDefaultOptions allocator;
    auto input_name = session.GetInputNameAllocated(0, allocator);
    auto output_name = session.GetOutputNameAllocated(0, allocator);
    const char* input_names[] = {input_name.get()};
    const char* output_names[] = {output_name.get()};
    const Ort::RunOptions run_options{nullptr};
    constexpr size_t input_count = 1;
    constexpr size_t output_count = 1;

    // 9. Run synchronous CPU inference. This call blocks until output is ready.
    // Arguments, in order:
    // - run_options: use default options for this inference call.
    // - input_names: names of the model nodes receiving input tensors.
    // - &input: address of the first input tensor.
    // - input_count: number of input names and tensors.
    // - output_names: names of the model output nodes to request.
    // - output_count: number of requested outputs.
    // The returned vector owns the output tensors allocated by ONNX Runtime.
    std::vector<Ort::Value> outputs = session.Run(
        run_options,
        input_names,
        &input,
        input_count,
        output_names,
        output_count
    );

    // 10. Validate the raw YOLOv8 output layout before reading its memory.
    const std::vector<int64_t> output_shape = outputs[0]
        .GetTensorTypeAndShapeInfo()
        .GetShape();
    if (output_shape.size() != 3 || output_shape[0] != 1 ||
        output_shape[1] < 5 || output_shape[2] <= 0) {
        throw std::runtime_error(
            "expected raw YOLOv8 output [1, 4 + classes, candidates]; export with nms=False"
        );
    }

    // 11. Decode candidates, restore source coordinates, and apply NMS.
    constexpr float confidence_threshold = 0.25F;
    constexpr float iou_threshold = 0.45F;
    const std::vector<Detection> detections = postprocess(
        outputs[0].GetTensorData<float>(),
        output_shape[1],
        output_shape[2],
        letterbox,
        image.size(),
        confidence_threshold,
        iou_threshold
    );

    // 12. Print the final detections in original-image pixel coordinates.
    std::cout << "detections: " << detections.size() << '\n';
    for (const Detection& detection : detections) {
        std::cout
            << "class=" << detection.class_id
            << " confidence=" << detection.confidence
            << " x=" << detection.box.x
            << " y=" << detection.box.y
            << " width=" << detection.box.width
            << " height=" << detection.box.height
            << '\n';
    }
    return 0;
} catch (const Ort::Exception& error) {
    std::cerr << "ONNX Runtime error: " << error.what() << '\n';
    return 1;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
