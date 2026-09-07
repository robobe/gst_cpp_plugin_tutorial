/*
 * Minimal ONNX Runtime inference pipeline
 * ---------------------------------------
 *
 * This program runs the model created by create_minimal_model.py. The model
 * accepts one FP32 tensor shaped [1, 3] and adds 1 to every value:
 *
 *   model file -> session -> input/output metadata -> input tensor
 *              -> session.Run() -> output tensor -> printed values
 *
 * ONNX Runtime executes the graph. The application is responsible for loading
 * the model, supplying correctly typed and shaped memory, choosing tensor
 * names, and keeping the input memory alive until inference finishes.
 *
 * Run without arguments to use onnx/minimal.onnx, or pass another compatible
 * model path as argv[1].
 */

#include <onnxruntime_cxx_api.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef MINIMAL_MODEL_PATH
#define MINIMAL_MODEL_PATH "../../minimal.onnx"
#endif

int main(int argc, char* argv[])
{
    try {
        // Step 1: Choose the ONNX file supplied by the user or the demo default.
        const std::string model_path = argc > 1 ? argv[1] : MINIMAL_MODEL_PATH;

        // Step 2: Create the runtime environment and load the model into a session.
        Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "minimal-model");
        Ort::SessionOptions options;
        Ort::Session session(environment, model_path.c_str(), options);

        // Step 3: Validate the simple one-input, one-output model interface.
        if (session.GetInputCount() != 1 || session.GetOutputCount() != 1) {
            throw std::runtime_error("expected one model input and one output");
        }

        // Step 4: Fetch names from the model instead of hard-coding "input"/"output".
        Ort::AllocatorWithDefaultOptions allocator;
        const auto input_name = session.GetInputNameAllocated(0, allocator);
        const auto output_name = session.GetOutputNameAllocated(0, allocator);

        // Step 5: Read and validate the input element type and dimensions.
        const Ort::TypeInfo input_type = session.GetInputTypeInfo(0);
        const auto input_tensor_info = input_type.GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> input_shape = input_tensor_info.GetShape();
        if (input_tensor_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            input_shape != std::vector<int64_t>({1, 3})) {
            throw std::runtime_error("expected FP32 input shape [1, 3]");
        }

        // Step 6: Create the input values and describe their CPU memory to the runtime.
        // CreateTensor does not copy input_data, so input_data must remain alive.
        std::array<float, 3> input_data = {1.0F, 2.0F, 3.0F};
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator,
            OrtMemTypeDefault
        );
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory,
            input_data.data(),
            input_data.size(),
            input_shape.data(),
            input_shape.size()
        );

        // Step 7: Run one synchronous inference.
        //
        // - RunOptions: per-run settings; nullptr means defaults.
        // - input_names/input_tensor/1: names, values, and count of model inputs.
        // - output_names/1: requested output names and their count.
        const char* input_names[] = {input_name.get()};
        const char* output_names[] = {output_name.get()};
        std::vector<Ort::Value> outputs = session.Run(
            Ort::RunOptions{nullptr},
            input_names,
            &input_tensor,
            1,
            output_names,
            1
        );

        // Step 8: Access the output tensor owned by outputs and print the result.
        const float* output_data = outputs[0].GetTensorData<float>();

        std::cout << "input name:  " << input_name.get() << '\n';
        std::cout << "output name: " << output_name.get() << '\n';
        std::cout << "input:  [" << input_data[0] << ", " << input_data[1]
                  << ", " << input_data[2] << "]\n";
        std::cout << "output: [" << output_data[0] << ", " << output_data[1]
                  << ", " << output_data[2] << "]\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
