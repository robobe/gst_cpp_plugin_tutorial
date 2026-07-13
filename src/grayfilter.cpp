#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>

#include <opencv2/imgproc.hpp>

#ifndef PACKAGE
#define PACKAGE "grayfilter"
#endif

typedef struct _GstGrayFilter {
    GstBaseTransform parent;
    GstVideoInfo video_info;
} GstGrayFilter;

typedef struct _GstGrayFilterClass {
    GstBaseTransformClass parent_class;
} GstGrayFilterClass;

#define GST_TYPE_GRAY_FILTER (gst_gray_filter_get_type())

G_DEFINE_TYPE(
    GstGrayFilter,
    gst_gray_filter,
    GST_TYPE_BASE_TRANSFORM
)

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

static gboolean gst_gray_filter_set_caps(
    GstBaseTransform* base,
    GstCaps* input_caps,
    GstCaps* output_caps)
{
    (void)output_caps;

    GstGrayFilter* self =
        reinterpret_cast<GstGrayFilter*>(base);

    return gst_video_info_from_caps(
        &self->video_info,
        input_caps
    );
}

static GstFlowReturn gst_gray_filter_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
{
    GstGrayFilter* self =
        reinterpret_cast<GstGrayFilter*>(base);

    GstVideoFrame video_frame;
    if (!gst_video_frame_map(
            &video_frame,
            &self->video_info,
            buffer,
            GST_MAP_READWRITE)) {
        GST_WARNING_OBJECT(base, "failed to map video frame");
        return GST_FLOW_ERROR;
    }

    guint8* pixels =
        static_cast<guint8*>(
            GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0)
        );

    const gint width =
        GST_VIDEO_FRAME_WIDTH(&video_frame);

    const gint height =
        GST_VIDEO_FRAME_HEIGHT(&video_frame);

    const gsize stride =
        GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);

    cv::Mat rgb(
        height,
        width,
        CV_8UC3,
        pixels,
        stride
    );

    cv::Mat gray;
    cv::cvtColor(
        rgb,
        gray,
        cv::COLOR_RGB2GRAY
    );

    cv::cvtColor(
        gray,
        rgb,
        cv::COLOR_GRAY2RGB
    );

    gst_video_frame_unmap(&video_frame);

    return GST_FLOW_OK;
}

static void gst_gray_filter_class_init(
    GstGrayFilterClass* klass)
{
    GstElementClass* element_class =
        GST_ELEMENT_CLASS(klass);

    GstBaseTransformClass* transform_class =
        GST_BASE_TRANSFORM_CLASS(klass);

    gst_element_class_set_static_metadata(
        element_class,
        "GrayFilter",
        "Filter/Video",
        "Converts RGB video buffers to grayscale with OpenCV",
        "example"
    );

    gst_element_class_add_static_pad_template(
        element_class,
        &sink_template
    );

    gst_element_class_add_static_pad_template(
        element_class,
        &src_template
    );

    transform_class->set_caps =
        GST_DEBUG_FUNCPTR(gst_gray_filter_set_caps);

    transform_class->transform_ip =
        GST_DEBUG_FUNCPTR(gst_gray_filter_transform_ip);
}

static void gst_gray_filter_init(
    GstGrayFilter* self)
{
    gst_video_info_init(&self->video_info);

    gst_base_transform_set_in_place(
        GST_BASE_TRANSFORM(self),
        TRUE
    );

    gst_base_transform_set_passthrough(
        GST_BASE_TRANSFORM(self),
        FALSE
    );
}

static gboolean plugin_init(GstPlugin* plugin)
{
    return gst_element_register(
        plugin,
        "grayfilter",
        GST_RANK_NONE,
        GST_TYPE_GRAY_FILTER
    );
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    grayfilter,
    "OpenCV grayscale video filter",
    plugin_init,
    "1.0",
    "LGPL",
    "grayfilter",
    "https://example.com"
)
