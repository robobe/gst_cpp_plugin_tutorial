#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

#ifndef PACKAGE
#define PACKAGE "simplevideosrc"
#endif

static constexpr gint kWidth = 320;
static constexpr gint kHeight = 240;
static constexpr gint kFps = 30;
static constexpr gint kChannels = 3;
static constexpr gsize kFrameSize = kWidth * kHeight * kChannels;

typedef struct _GstSimpleVideoSrc {
    GstPushSrc parent;
    guint64 frame_number;
} GstSimpleVideoSrc;

typedef struct _GstSimpleVideoSrcClass {
    GstPushSrcClass parent_class;
} GstSimpleVideoSrcClass;

#define GST_TYPE_SIMPLE_VIDEO_SRC (gst_simple_video_src_get_type())

G_DEFINE_TYPE(
    GstSimpleVideoSrc,
    gst_simple_video_src,
    GST_TYPE_PUSH_SRC
)

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            "video/x-raw,"
            "format=RGB,"
            "width=320,"
            "height=240,"
            "framerate=30/1"
        )
    );

static void fill_rgb_frame(
    guint8* data,
    guint64 frame_number)
{
    for (gint y = 0; y < kHeight; y++) {
        for (gint x = 0; x < kWidth; x++) {
            const gsize offset =
                (static_cast<gsize>(y) * kWidth + x) * kChannels;

            data[offset + 0] =
                static_cast<guint8>((x + frame_number * 3) % 256);
            data[offset + 1] =
                static_cast<guint8>((y + frame_number * 2) % 256);
            data[offset + 2] =
                static_cast<guint8>((x + y + frame_number * 5) % 256);
        }
    }
}

static GstFlowReturn gst_simple_video_src_create(
    GstPushSrc* src,
    GstBuffer** buffer)
{
    GstSimpleVideoSrc* self =
        reinterpret_cast<GstSimpleVideoSrc*>(src);

    GstBuffer* out_buffer =
        gst_buffer_new_allocate(nullptr, kFrameSize, nullptr);

    if (out_buffer == nullptr) {
        return GST_FLOW_ERROR;
    }

    GstMapInfo map_info;
    if (!gst_buffer_map(out_buffer, &map_info, GST_MAP_WRITE)) {
        gst_buffer_unref(out_buffer);
        return GST_FLOW_ERROR;
    }

    fill_rgb_frame(map_info.data, self->frame_number);
    gst_buffer_unmap(out_buffer, &map_info);

    const GstClockTime duration =
        gst_util_uint64_scale_int(GST_SECOND, 1, kFps);

    GST_BUFFER_PTS(out_buffer) =
        self->frame_number * duration;
    GST_BUFFER_DTS(out_buffer) =
        GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(out_buffer) =
        duration;
    GST_BUFFER_OFFSET(out_buffer) =
        self->frame_number;

    self->frame_number++;
    *buffer = out_buffer;

    return GST_FLOW_OK;
}

static void gst_simple_video_src_class_init(
    GstSimpleVideoSrcClass* klass)
{
    GstElementClass* element_class =
        GST_ELEMENT_CLASS(klass);

    GstPushSrcClass* push_src_class =
        GST_PUSH_SRC_CLASS(klass);

    gst_element_class_set_static_metadata(
        element_class,
        "SimpleVideoSrc",
        "Source/Video",
        "Generates RGB video frames in C++",
        "example"
    );

    gst_element_class_add_static_pad_template(
        element_class,
        &src_template
    );

    push_src_class->create =
        GST_DEBUG_FUNCPTR(gst_simple_video_src_create);
}

static void gst_simple_video_src_init(
    GstSimpleVideoSrc* self)
{
    self->frame_number = 0;

    gst_base_src_set_format(
        GST_BASE_SRC(self),
        GST_FORMAT_TIME
    );

    gst_base_src_set_live(
        GST_BASE_SRC(self),
        FALSE
    );
}

static gboolean plugin_init(GstPlugin* plugin)
{
    return gst_element_register(
        plugin,
        "simplevideosrc",
        GST_RANK_NONE,
        GST_TYPE_SIMPLE_VIDEO_SRC
    );
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    simplevideosrc,
    "Minimal generated video source",
    plugin_init,
    "1.0",
    "LGPL",
    "simplevideosrc",
    "https://example.com"
)
