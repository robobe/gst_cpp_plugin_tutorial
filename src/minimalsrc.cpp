// GST_PLUGIN_PATH="$PWD/build" gst-inspect-1.0 minimalsrc
// GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 minimalsrc num-buffers=5 ! fakesink

#include <gst/base/gstpushsrc.h>
#include <gst/gst.h>

#ifndef PACKAGE
#define PACKAGE "minimalsrc"
#endif

namespace {

constexpr guint kWidth = 320;
constexpr guint kHeight = 240;
constexpr guint kBytesPerPixel = 3;
constexpr guint kFramesPerSecond = 30;
constexpr gsize kFrameSize = kWidth * kHeight * kBytesPerPixel;

}  // namespace

typedef struct _GstMinimalSrc {
    GstPushSrc parent;
    guint64 frame_number;
} GstMinimalSrc;

typedef struct _GstMinimalSrcClass {
    GstPushSrcClass parent_class;
} GstMinimalSrcClass;

#define GST_TYPE_MINIMAL_SRC (gst_minimal_src_get_type())

G_DEFINE_TYPE(GstMinimalSrc, gst_minimal_src, GST_TYPE_PUSH_SRC)

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw,"
        "format=RGB,"
        "width=320,"
        "height=240,"
        "framerate=30/1"));

static GstFlowReturn gst_minimal_src_create(
    GstPushSrc* push_src,
    GstBuffer** output)
{
    auto* self = reinterpret_cast<GstMinimalSrc*>(push_src);
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, kFrameSize, nullptr);

    if (buffer == nullptr) {
        return GST_FLOW_ERROR;
    }

    // RGB zeroes represent a solid black frame.
    gst_buffer_memset(buffer, 0, 0, kFrameSize);

    const GstClockTime frame_duration =
        gst_util_uint64_scale_int(GST_SECOND, 1, kFramesPerSecond);

    GST_BUFFER_PTS(buffer) = self->frame_number * frame_duration;
    GST_BUFFER_DURATION(buffer) = frame_duration;
    GST_BUFFER_OFFSET(buffer) = self->frame_number;

    ++self->frame_number;
    *output = buffer;
    return GST_FLOW_OK;
}

static void gst_minimal_src_class_init(GstMinimalSrcClass* klass)
{
    auto* element_class = GST_ELEMENT_CLASS(klass);
    auto* push_src_class = GST_PUSH_SRC_CLASS(klass);

    gst_element_class_set_static_metadata(
        element_class,
        "Minimal Source",
        "Source/Video",
        "Generates black RGB video frames",
        "example");
    gst_element_class_add_static_pad_template(element_class, &src_template);

    push_src_class->create = GST_DEBUG_FUNCPTR(gst_minimal_src_create);
}

static void gst_minimal_src_init(GstMinimalSrc* self)
{
    self->frame_number = 0;
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_live(GST_BASE_SRC(self), FALSE);
}

static gboolean plugin_init(GstPlugin* plugin)
{
    return gst_element_register(
        plugin,
        "minimalsrc",
        GST_RANK_NONE,
        GST_TYPE_MINIMAL_SRC);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    minimalsrc,
    "Minimal C++ video source",
    plugin_init,
    "1.0",
    "LGPL",
    "minimalsrc",
    "https://example.com")
