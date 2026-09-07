/*
 * NanoTrackV3 RK3566 C++ pipeline.
 *
 * Data path:
 *
 * OpenCV BGR frame
 *      |
 *      v
 * OpenCV padded crop
 *      |
 *      v
 * RGA importbuffer_virtualaddr()
 *      |
 *      v
 * RGA resize
 *      |
 *      v
 * RKNN DMA-BUF imported with importbuffer_fd()
 *      |
 *      v
 * RKNN backbone
 *      |
 *      v
 * NanoTrack head
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <rknn_api.h>

#include <rga/im2d.h>
#include <rga/rga.h>

namespace {

using Clock = std::chrono::steady_clock;

bool use_rga = true;

// -----------------------------------------------------------------------------
// Fatal signal handling
// -----------------------------------------------------------------------------

void fatal_signal(int signal) {
    static constexpr char message[] =
        "Fatal process signal; exiting immediately. Try --resize cpu.\n";

    const auto ignored =
        ::write(STDERR_FILENO, message, sizeof(message) - 1);

    (void)ignored;

    ::_exit(128 + signal);
}

void install_signal_handlers() {
    struct sigaction action {};

    action.sa_handler = fatal_signal;
    sigemptyset(&action.sa_mask);

    for (int signal :
         {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE}) {

        if (sigaction(signal, &action, nullptr) != 0) {
            throw std::runtime_error(
                "Cannot install fatal signal handler");
        }
    }
}

// -----------------------------------------------------------------------------
// General helpers
// -----------------------------------------------------------------------------

void check(int result, const std::string& action) {
    if (result < 0) {
        throw std::runtime_error(
            action + ": " + std::to_string(result));
    }
}

std::vector<unsigned char>
read_file(const std::string& path) {

    std::ifstream stream(
        path,
        std::ios::binary | std::ios::ate);

    if (!stream) {
        throw std::runtime_error(
            "Cannot open model: " + path);
    }

    const auto size = stream.tellg();

    std::vector<unsigned char> data(
        static_cast<size_t>(size));

    stream.seekg(0);

    if (!stream.read(
            reinterpret_cast<char*>(data.data()),
            size)) {

        throw std::runtime_error(
            "Cannot read model: " + path);
    }

    return data;
}

// -----------------------------------------------------------------------------
// Small RAII wrapper for RGA imported buffers
// -----------------------------------------------------------------------------

class RgaImportedBuffer {
public:
    RgaImportedBuffer() = default;

    explicit RgaImportedBuffer(
        rga_buffer_handle_t handle)
        : handle_(handle) {
    }

    ~RgaImportedBuffer() {
        reset();
    }

    RgaImportedBuffer(
        const RgaImportedBuffer&) = delete;

    RgaImportedBuffer&
    operator=(const RgaImportedBuffer&) = delete;

    RgaImportedBuffer(
        RgaImportedBuffer&& other) noexcept
        : handle_(other.handle_) {

        other.handle_ = 0;
    }

    RgaImportedBuffer&
    operator=(RgaImportedBuffer&& other) noexcept {

        if (this != &other) {
            reset();

            handle_ = other.handle_;
            other.handle_ = 0;
        }

        return *this;
    }

    bool valid() const {
        return handle_ != 0;
    }

    rga_buffer_handle_t get() const {
        return handle_;
    }

    void reset() {
        if (handle_ != 0) {
            releasebuffer_handle(handle_);
            handle_ = 0;
        }
    }

private:
    rga_buffer_handle_t handle_ = 0;
};

// -----------------------------------------------------------------------------
// RKNN model wrapper
// -----------------------------------------------------------------------------

class Model {
public:
    explicit Model(const std::string& path) {

        auto bytes = read_file(path);

        check(
            rknn_init(
                &ctx_,
                bytes.data(),
                bytes.size(),
                0,
                nullptr),
            "rknn_init " + path);

        try {

            check(
                rknn_query(
                    ctx_,
                    RKNN_QUERY_IN_OUT_NUM,
                    &io_,
                    sizeof(io_)),
                "query I/O count");

            inputs_.resize(io_.n_input);
            outputs_.resize(io_.n_output);

            input_mem_.resize(
                io_.n_input,
                nullptr);

            output_mem_.resize(
                io_.n_output,
                nullptr);

            for (uint32_t i = 0;
                 i < io_.n_input;
                 ++i) {

                inputs_[i].index = i;

                check(
                    rknn_query(
                        ctx_,
                        RKNN_QUERY_INPUT_ATTR,
                        &inputs_[i],
                        sizeof(inputs_[i])),
                    "query input");
            }

            for (uint32_t i = 0;
                 i < io_.n_output;
                 ++i) {

                outputs_[i].index = i;

                check(
                    rknn_query(
                        ctx_,
                        RKNN_QUERY_OUTPUT_ATTR,
                        &outputs_[i],
                        sizeof(outputs_[i])),
                    "query output");
            }
        }
        catch (...) {
            release();
            throw;
        }
    }

    ~Model() {
        release();
    }

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    void release() noexcept {

        for (auto* memory : imported_mem_) {
            if (memory) {
                rknn_destroy_mem(ctx_, memory);
            }
        }

        for (auto* memory : input_mem_) {
            if (memory) {
                rknn_destroy_mem(ctx_, memory);
            }
        }

        for (auto* memory : output_mem_) {
            if (memory) {
                rknn_destroy_mem(ctx_, memory);
            }
        }

        if (ctx_) {
            rknn_destroy(ctx_);
        }

        ctx_ = 0;
    }

    // -------------------------------------------------------------------------
    // Allocate UINT8 NHWC image input
    // -------------------------------------------------------------------------

    void allocate_image_input(
        int index,
        int width,
        int height) {

        auto attr = inputs_.at(index);

        const uint32_t logical_size =
            width * height * 3;

        // Keep your original 16-pixel RKNN allocation alignment.
        const uint32_t aligned_width =
            (width + 15) & ~15;

        const uint32_t aligned_height =
            (height + 15) & ~15;

        const uint32_t aligned_size =
            aligned_width *
            aligned_height *
            3;

        attr.type = RKNN_TENSOR_UINT8;
        attr.fmt = RKNN_TENSOR_NHWC;
        attr.pass_through = 0;

        attr.size = logical_size;
        attr.size_with_stride = aligned_size;

        attr.w_stride = aligned_width;
        attr.h_stride = aligned_height;

        auto* memory =
            rknn_create_mem(
                ctx_,
                aligned_size);

        if (!memory) {
            throw std::runtime_error(
                "rknn_create_mem image input failed");
        }

        input_mem_[index] = memory;

        std::memset(
            memory->virt_addr,
            0,
            aligned_size);

        check(
            rknn_set_io_mem(
                ctx_,
                memory,
                &attr),
            "bind image input");

        inputs_[index] = attr;

        std::cout
            << "RKNN image input:"
            << "\n  logical    = "
            << width << "x" << height
            << "\n  stride     = "
            << aligned_width
            << "x"
            << aligned_height
            << "\n  bytes      = "
            << aligned_size
            << "\n  fd         = "
            << memory->fd
            << "\n  virt_addr  = "
            << memory->virt_addr
            << "\n";
    }

    // -------------------------------------------------------------------------
    // Allocate FLOAT32 output
    // -------------------------------------------------------------------------

    void allocate_float_output(
        int index,
        rknn_tensor_format format =
            RKNN_TENSOR_NCHW) {

        auto attr =
            outputs_.at(index);

        attr.type =
            RKNN_TENSOR_FLOAT32;

        attr.fmt = format;

        attr.size =
            attr.n_elems *
            sizeof(float);

        attr.size_with_stride =
            attr.size;

        auto* memory =
            rknn_create_mem(
                ctx_,
                attr.size);

        if (!memory) {
            throw std::runtime_error(
                "rknn_create_mem output failed");
        }

        output_mem_[index] =
            memory;

        check(
            rknn_set_io_mem(
                ctx_,
                memory,
                &attr),
            "bind output");

        outputs_[index] =
            attr;
    }

    // -------------------------------------------------------------------------
    // Import backbone output as tracking-head input
    // -------------------------------------------------------------------------

    void import_float_input(
        int index,
        rknn_tensor_mem* source,
        uint32_t elements) {

        auto attr =
            inputs_.at(index);

        attr.type =
            RKNN_TENSOR_FLOAT32;

        attr.fmt =
            RKNN_TENSOR_NHWC;

        attr.pass_through = 0;

        attr.size =
            elements *
            sizeof(float);

        attr.size_with_stride =
            attr.size;

        auto* memory =
            rknn_create_mem_from_fd(
                ctx_,
                source->fd,
                source->virt_addr,
                attr.size,
                source->offset);

        if (!memory) {
            throw std::runtime_error(
                "import shared feature memory failed");
        }

        imported_mem_.push_back(
            memory);

        check(
            rknn_set_io_mem(
                ctx_,
                memory,
                &attr),
            "bind shared feature input");
    }

    void run() {
        check(
            rknn_run(
                ctx_,
                nullptr),
            "rknn_run");
    }

    rknn_context context() const {
        return ctx_;
    }

    rknn_tensor_mem*
    input(int i) const {
        return input_mem_.at(i);
    }

    rknn_tensor_mem*
    output(int i) const {
        return output_mem_.at(i);
    }

    uint32_t
    output_elements(int i) const {
        return outputs_.at(i).n_elems;
    }

private:
    rknn_context ctx_ = 0;

    rknn_input_output_num io_{};

    std::vector<rknn_tensor_attr>
        inputs_;

    std::vector<rknn_tensor_attr>
        outputs_;

    std::vector<rknn_tensor_mem*>
        input_mem_;

    std::vector<rknn_tensor_mem*>
        output_mem_;

    std::vector<rknn_tensor_mem*>
        imported_mem_;
};

// -----------------------------------------------------------------------------
// RGA crop + resize
// -----------------------------------------------------------------------------

void rga_crop_resize(
    const cv::Mat& frame,
    cv::Point2f center,
    int original_size,
    int output_size,
    const cv::Scalar& average,
    rknn_context ctx,
    rknn_tensor_mem* destination) {

    if (frame.empty() ||
        frame.type() != CV_8UC3 ||
        original_size <= 0 ||
        output_size <= 0 ||
        !destination ||
        !destination->virt_addr) {

        throw std::runtime_error(
            "Invalid resize input or destination");
    }

    // -------------------------------------------------------------------------
    // Construct Siamese crop
    // -------------------------------------------------------------------------

    const int x0 =
        static_cast<int>(
            std::floor(
                center.x -
                original_size * 0.5f));

    const int y0 =
        static_cast<int>(
            std::floor(
                center.y -
                original_size * 0.5f));

    // RGA2 requires byte stride alignment.
    // BGR888 => 3 bytes / pixel.
    //
    // Keeping width aligned to 4 pixels gives:
    //
    // stride_bytes = width_stride * 3
    //
    const int source_stride =
        (original_size + 3) & ~3;

    cv::Mat padded(
        original_size,
        source_stride,
        CV_8UC3,
        average);

    const cv::Rect requested(
        x0,
        y0,
        original_size,
        original_size);

    const cv::Rect image_bounds(
        0,
        0,
        frame.cols,
        frame.rows);

    const cv::Rect valid =
        requested & image_bounds;

    if (valid.empty()) {
        throw std::runtime_error(
            "Search crop is outside the frame");
    }

    frame(valid).copyTo(
        padded(
            cv::Rect(
                valid.x - x0,
                valid.y - y0,
                valid.width,
                valid.height)));

    // RKNN allocation stride.
    const int aligned =
        (output_size + 15) & ~15;

    const size_t src_size =
        static_cast<size_t>(
            source_stride) *
        original_size *
        3;

    const size_t dst_size =
        static_cast<size_t>(
            aligned) *
        aligned *
        3;

    if (destination->size < dst_size) {
        throw std::runtime_error(
            "RKNN image destination smaller than RGA stride");
    }

    // -------------------------------------------------------------------------
    // Try RGA
    // -------------------------------------------------------------------------

    if (use_rga) {

        /*
         * Important:
         *
         * Explicitly import both buffers into librga.
         *
         * Source:
         *   regular CPU virtual memory
         *
         * Destination:
         *   DMA-BUF created by rknn_create_mem()
         */

        RgaImportedBuffer src_handle(
            importbuffer_virtualaddr(
                padded.data,
                static_cast<int>(src_size)));

        if (!src_handle.valid()) {

            std::cerr
                << "RGA importbuffer_virtualaddr failed; "
                << "switching to CPU resize.\n";

            use_rga = false;
        }

        if (use_rga) {

            RgaImportedBuffer dst_handle(
                importbuffer_fd(
                    destination->fd,
                    static_cast<int>(dst_size)));

            if (!dst_handle.valid()) {

                std::cerr
                    << "RGA importbuffer_fd failed for RKNN fd="
                    << destination->fd
                    << "; switching to CPU resize.\n";

                use_rga = false;
            }

            if (use_rga) {

                auto src =
                    wrapbuffer_handle(
                        src_handle.get(),
                        original_size,
                        original_size,
                        RK_FORMAT_BGR_888,
                        source_stride,
                        original_size);

                auto dst =
                    wrapbuffer_handle(
                        dst_handle.get(),
                        output_size,
                        output_size,
                        RK_FORMAT_BGR_888,
                        aligned,
                        aligned);

                std::cout
                    << "RGA resize:"
                    << " src="
                    << original_size
                    << "x"
                    << original_size
                    << " stride="
                    << source_stride
                    << " dst="
                    << output_size
                    << "x"
                    << output_size
                    << " stride="
                    << aligned
                    << " fd="
                    << destination->fd
                    << '\n';

                /*
                 * Make sure previous CPU initialization of the
                 * RKNN buffer is visible before RGA starts using it.
                 */
                check(
                    rknn_mem_sync(
                        ctx,
                        destination,
                        RKNN_MEMORY_SYNC_TO_DEVICE),
                    "sync before RGA");

                const IM_STATUS status =
                    imresize(
                        src,
                        dst);

                if (status == IM_STATUS_SUCCESS) {

                    /*
                     * Do NOT do RKNN_MEMORY_SYNC_TO_DEVICE here.
                     *
                     * RGA has just written the DMA buffer. Flushing
                     * stale CPU cache after that can overwrite data
                     * written by RGA.
                     */

                    return;
                }

                std::cerr
                    << "RGA resize failed: "
                    << imStrError(status)
                    << "; switching to CPU resize for "
                       "the rest of this run.\n";

                use_rga = false;
            }
        }
    }

    // -------------------------------------------------------------------------
    // CPU fallback
    // -------------------------------------------------------------------------

    /*
     * If RGA partially touched the buffer, invalidate the CPU
     * mapping before we overwrite it.
     */
    check(
        rknn_mem_sync(
            ctx,
            destination,
            RKNN_MEMORY_SYNC_FROM_DEVICE),
        "sync before CPU resize");

    std::memset(
        destination->virt_addr,
        0,
        dst_size);

    /*
     * Important:
     *
     * logical output is:
     *
     *   127x127
     * or
     *   255x255
     *
     * while row stride remains:
     *
     *   128
     * or
     *   256
     */
    cv::Mat output(
        output_size,
        output_size,
        CV_8UC3,
        destination->virt_addr,
        static_cast<size_t>(aligned) * 3);

    cv::resize(
        padded(
            cv::Rect(
                0,
                0,
                original_size,
                original_size)),
        output,
        output.size(),
        0,
        0,
        cv::INTER_LINEAR);

    check(
        rknn_mem_sync(
            ctx,
            destination,
            RKNN_MEMORY_SYNC_TO_DEVICE),
        "sync CPU input");
}

// -----------------------------------------------------------------------------
// NanoTrack helpers
// -----------------------------------------------------------------------------

float padded_size(
    float w,
    float h) {

    const float pad =
        (w + h) * 0.5f;

    return std::sqrt(
        (w + pad) *
        (h + pad));
}

// -----------------------------------------------------------------------------
// Command-line options
// -----------------------------------------------------------------------------

struct Options {

    std::string video =
        "assets/camera_run_2s_10s.mp4";

    std::string models =
        "rknn/models";

    std::string precision =
        "mixed";

    bool display = true;

    std::string boxes;

    cv::Rect2d roi;

    bool has_roi = false;
};

Options parse_args(
    int argc,
    char** argv) {

    Options out;

    for (int i = 1;
         i < argc;
         ++i) {

        const std::string arg =
            argv[i];

        auto value =
            [&]() -> std::string {

            if (++i >= argc) {
                throw std::runtime_error(
                    "Missing value after " + arg);
            }

            return argv[i];
        };

        if (arg == "--video") {

            out.video =
                value();
        }
        else if (arg == "--models") {

            out.models =
                value();
        }
        else if (arg == "--precision") {

            out.precision =
                value();
        }
        else if (arg == "--roi") {

            const std::string text =
                value();

            if (std::sscanf(
                    text.c_str(),
                    "%lf,%lf,%lf,%lf",
                    &out.roi.x,
                    &out.roi.y,
                    &out.roi.width,
                    &out.roi.height) != 4 ||
                out.roi.width <= 0 ||
                out.roi.height <= 0) {

                throw std::runtime_error(
                    "--roi must be x,y,width,height");
            }

            out.has_roi =
                true;
        }
        else if (arg == "--resize") {

            const auto mode =
                value();

            if (mode != "auto" &&
                mode != "cpu") {

                throw std::runtime_error(
                    "--resize must be auto or cpu");
            }

            use_rga =
                mode == "auto";
        }
        else if (arg == "--no-display") {

            out.display =
                false;
        }
        else if (arg == "--boxes") {

            out.boxes =
                value();
        }
        else if (arg == "--help") {

            std::cout
                << "Usage: nanotrack_rknn_rga "
                << "[--video PATH] "
                << "[--models DIR] "
                << "[--precision fp16|mixed|int8|int8-mmse] "
                << "[--roi x,y,w,h] "
                << "[--boxes FILE] "
                << "[--resize auto|cpu] "
                << "[--no-display]\n";

            std::exit(0);
        }
        else {

            throw std::runtime_error(
                "Unknown option: " + arg);
        }
    }

    return out;
}

std::string model_path(
    const Options& o,
    const std::string& stem,
    bool head) {

    std::string suffix;

    if (o.precision == "int8") {

        suffix =
            "_int8";
    }
    else if (o.precision == "int8-mmse") {

        suffix =
            "_int8_mmse";
    }
    else if (o.precision == "mixed" &&
             head) {

        suffix =
            "_int8";
    }
    else if (o.precision != "fp16" &&
             o.precision != "mixed") {

        throw std::runtime_error(
            "Invalid precision: " +
            o.precision);
    }

    return
        o.models +
        "/" +
        stem +
        suffix +
        ".rknn";
}

} // namespace

// =============================================================================
// main
// =============================================================================

int main(
    int argc,
    char** argv) try {

    install_signal_handlers();

    const Options options =
        parse_args(
            argc,
            argv);

    // -------------------------------------------------------------------------
    // Load RKNN models
    // -------------------------------------------------------------------------

    Model template_net(
        model_path(
            options,
            "nanotrack_backbone_template",
            false));

    Model search_net(
        model_path(
            options,
            "nanotrack_backbone",
            false));

    Model head(
        model_path(
            options,
            "nanotrack_head",
            true));

    // -------------------------------------------------------------------------
    // Allocate persistent RKNN memory
    // -------------------------------------------------------------------------

    template_net.allocate_image_input(
        0,
        127,
        127);

    search_net.allocate_image_input(
        0,
        255,
        255);

    template_net.allocate_float_output(
        0,
        RKNN_TENSOR_NHWC);

    search_net.allocate_float_output(
        0,
        RKNN_TENSOR_NHWC);

    head.import_float_input(
        0,
        template_net.output(0),
        template_net.output_elements(0));

    head.import_float_input(
        1,
        search_net.output(0),
        search_net.output_elements(0));

    head.allocate_float_output(0);
    head.allocate_float_output(1);

    // -------------------------------------------------------------------------
    // Video
    // -------------------------------------------------------------------------

    cv::VideoCapture capture(
        options.video);

    cv::Mat frame;

    if (!capture.isOpened() ||
        !capture.read(frame)) {

        throw std::runtime_error(
            "Cannot open video");
    }

    cv::Rect2d roi =
        options.roi;

    if (!options.has_roi) {

        roi =
            cv::Rect2d(
                cv::selectROI(
                    "NanoTrack C++ RGA",
                    frame,
                    true,
                    false));
    }

    if (roi.width <= 0 ||
        roi.height <= 0) {

        return 0;
    }

    cv::Point2f center(
        roi.x +
            (roi.width - 1) *
                0.5,

        roi.y +
            (roi.height - 1) *
                0.5);

    cv::Size2f size(
        roi.width,
        roi.height);

    const cv::Scalar average =
        cv::mean(frame);

    // -------------------------------------------------------------------------
    // Template initialization
    // -------------------------------------------------------------------------

    const float context_w =
        size.width +
        0.5f *
            (size.width +
             size.height);

    const float context_h =
        size.height +
        0.5f *
            (size.width +
             size.height);

    rga_crop_resize(
        frame,
        center,
        std::lround(
            std::sqrt(
                context_w *
                context_h)),
        127,
        average,
        template_net.context(),
        template_net.input(0));

    template_net.run();

    // -------------------------------------------------------------------------
    // NanoTrack window/grid
    // -------------------------------------------------------------------------

    constexpr int side =
        15;

    constexpr int count =
        side * side;

    std::vector<float>
        window(count);

    std::vector<float>
        point_x(count);

    std::vector<float>
        point_y(count);

    for (int y = 0, k = 0;
         y < side;
         ++y) {

        for (int x = 0;
             x < side;
             ++x, ++k) {

            const float hx =
                0.5f -
                0.5f *
                    std::cos(
                        2.f *
                        float(M_PI) *
                        x /
                        (side - 1));

            const float hy =
                0.5f -
                0.5f *
                    std::cos(
                        2.f *
                        float(M_PI) *
                        y /
                        (side - 1));

            window[k] =
                hx * hy;

            point_x[k] =
                (x - side / 2) *
                16.f;

            point_y[k] =
                (y - side / 2) *
                16.f;
        }
    }

    // -------------------------------------------------------------------------
    // Tracking
    // -------------------------------------------------------------------------

    int frames =
        0;

    double tracking_seconds =
        0;

    std::ofstream box_stream;

    if (!options.boxes.empty()) {

        box_stream.open(
            options.boxes);

        if (!box_stream) {
            throw std::runtime_error(
                "Cannot open box output: " +
                options.boxes);
        }
    }

    while (capture.read(frame)) {

        const auto start =
            Clock::now();

        const float cw =
            size.width +
            0.5f *
                (size.width +
                 size.height);

        const float ch =
            size.height +
            0.5f *
                (size.width +
                 size.height);

        const float template_scale =
            std::sqrt(
                cw * ch);

        const float scale =
            127.f /
            template_scale;

        const int search_size =
            std::lround(
                template_scale *
                255.f /
                127.f);

        // ---------------------------------------------------------------------
        // Search image
        // ---------------------------------------------------------------------

        rga_crop_resize(
            frame,
            center,
            search_size,
            255,
            average,
            search_net.context(),
            search_net.input(0));

        search_net.run();

        head.run();

        // ---------------------------------------------------------------------
        // Read outputs
        // ---------------------------------------------------------------------

        check(
            rknn_mem_sync(
                head.context(),
                head.output(0),
                RKNN_MEMORY_SYNC_FROM_DEVICE),
            "sync cls");

        check(
            rknn_mem_sync(
                head.context(),
                head.output(1),
                RKNN_MEMORY_SYNC_FROM_DEVICE),
            "sync loc");

        const auto* cls =
            static_cast<const float*>(
                head.output(0)->virt_addr);

        const auto* loc =
            static_cast<const float*>(
                head.output(1)->virt_addr);

        int best =
            0;

        float best_rank =
            -1e30f;

        float best_score =
            0;

        float best_penalty =
            0;

        float bx = 0;
        float by = 0;
        float bw = 0;
        float bh = 0;

        for (int i = 0;
             i < count;
             ++i) {

            const float e0 =
                std::exp(
                    cls[i]);

            const float e1 =
                std::exp(
                    cls[count + i]);

            const float score =
                e1 /
                (e0 + e1);

            const float x1 =
                point_x[i] -
                loc[i];

            const float y1 =
                point_y[i] -
                loc[count + i];

            const float x2 =
                point_x[i] +
                loc[2 * count + i];

            const float y2 =
                point_y[i] +
                loc[3 * count + i];

            const float w =
                x2 - x1;

            const float h =
                y2 - y1;

            const float sr =
                padded_size(w, h) /
                padded_size(
                    size.width *
                        scale,
                    size.height *
                        scale);

            const float rr =
                (size.width /
                 size.height) /
                (w / h);

            const float penalty =
                std::exp(
                    -(
                        std::max(
                            sr,
                            1.f / sr) *
                        std::max(
                            rr,
                            1.f / rr) -
                        1.f) *
                    0.138f);

            const float rank =
                penalty *
                    score *
                    (1.f - 0.455f) +
                window[i] *
                    0.455f;

            if (rank >
                best_rank) {

                best =
                    i;

                best_rank =
                    rank;

                best_score =
                    score;

                best_penalty =
                    penalty;

                bx =
                    (x1 + x2) *
                    0.5f /
                    scale;

                by =
                    (y1 + y2) *
                    0.5f /
                    scale;

                bw =
                    w /
                    scale;

                bh =
                    h /
                    scale;
            }
        }

        (void)best;

        // ---------------------------------------------------------------------
        // State update
        // ---------------------------------------------------------------------

        const float lr =
            best_penalty *
            best_score *
            0.348f;

        center.x =
            std::clamp(
                center.x + bx,
                0.f,
                float(frame.cols));

        center.y =
            std::clamp(
                center.y + by,
                0.f,
                float(frame.rows));

        size.width =
            std::clamp(
                size.width *
                    (1 - lr) +
                    bw * lr,
                10.f,
                float(frame.cols));

        size.height =
            std::clamp(
                size.height *
                    (1 - lr) +
                    bh * lr,
                10.f,
                float(frame.rows));

        if (box_stream) {

            box_stream
                << center.x -
                       size.width / 2
                << ','
                << center.y -
                       size.height / 2
                << ','
                << size.width
                << ','
                << size.height
                << '\n';
        }

        // ---------------------------------------------------------------------
        // FPS
        // ---------------------------------------------------------------------

        const double elapsed =
            std::chrono::duration<double>(
                Clock::now() -
                start)
                .count();

        tracking_seconds +=
            elapsed;

        ++frames;

        // ---------------------------------------------------------------------
        // Display
        // ---------------------------------------------------------------------

        if (options.display) {

            cv::Rect box(
                std::lround(
                    center.x -
                    size.width / 2),

                std::lround(
                    center.y -
                    size.height / 2),

                std::lround(
                    size.width),

                std::lround(
                    size.height));

            cv::rectangle(
                frame,
                box,
                cv::Scalar(
                    0,
                    255,
                    0),
                2);

            cv::putText(
                frame,
                "FPS " +
                    std::to_string(
                        int(
                            frames /
                            tracking_seconds)),
                {15, 30},
                cv::FONT_HERSHEY_SIMPLEX,
                0.7,
                {0, 255, 255},
                2);

            cv::imshow(
                "NanoTrack C++ RGA",
                frame);

            const int key =
                cv::waitKey(1) &
                0xff;

            if (key == 27 ||
                key == 'q') {

                break;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Summary
    // -------------------------------------------------------------------------

    std::cout
        << "Processed frames: "
        << frames
        << "\nAverage tracking: "
        << tracking_seconds *
               1000 /
               std::max(
                   1,
                   frames)
        << " ms/frame"
        << "\nTracking FPS: "
        << frames /
               std::max(
                   1e-9,
                   tracking_seconds)
        << '\n';

    return 0;
}
catch (const std::exception& error) {

    std::cerr
        << "Error: "
        << error.what()
        << '\n';

    std::cerr.flush();

    std::_Exit(1);
}
catch (...) {

    static constexpr char message[] =
        "Error: unknown exception\n";

    const auto ignored =
        ::write(
            STDERR_FILENO,
            message,
            sizeof(message) - 1);

    (void)ignored;

    std::_Exit(1);
}
