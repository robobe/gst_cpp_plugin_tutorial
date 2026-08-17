#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

#ifndef PACKAGE
#define PACKAGE "colorboxsrc"
#endif

static constexpr gint kWidth = 320;
static constexpr gint kHeight = 240;
static constexpr gint kFps = 30;
static constexpr gint kChannels = 3;
static constexpr gint kBoxSize = 48;
static constexpr gsize kFrameSize = kWidth * kHeight * kChannels;

typedef struct _GstColorBoxSrc {
    GstPushSrc parent;
    guint64 frame_number;
    guint box_red;
    guint box_green;
    guint box_blue;
} GstColorBoxSrc;

typedef struct _GstColorBoxSrcClass {
    GstPushSrcClass parent_class;
} GstColorBoxSrcClass;

#define GST_TYPE_COLOR_BOX_SRC (gst_color_box_src_get_type())

G_DEFINE_TYPE(
    GstColorBoxSrc,
    gst_color_box_src,
    GST_TYPE_PUSH_SRC
)

enum {
    PROP_0,
    PROP_BOX_RED,
    PROP_BOX_GREEN,
    PROP_BOX_BLUE
};

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

static void gst_color_box_src_set_property(
    GObject* object,
    guint property_id,
    const GValue* value,
    GParamSpec* pspec)
{
    GstColorBoxSrc* self =
        reinterpret_cast<GstColorBoxSrc*>(object);

    if (property_id == PROP_BOX_RED) {
        self->box_red = g_value_get_uint(value);
        return;
    }

    if (property_id == PROP_BOX_GREEN) {
        self->box_green = g_value_get_uint(value);
        return;
    }

    if (property_id == PROP_BOX_BLUE) {
        self->box_blue = g_value_get_uint(value);
        return;
    }

    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
}

static void gst_color_box_src_get_property(
    GObject* object,
    guint property_id,
    GValue* value,
    GParamSpec* pspec)
{
    GstColorBoxSrc* self =
        reinterpret_cast<GstColorBoxSrc*>(object);

    if (property_id == PROP_BOX_RED) {
        g_value_set_uint(value, self->box_red);
        return;
    }

    if (property_id == PROP_BOX_GREEN) {
        g_value_set_uint(value, self->box_green);
        return;
    }

    if (property_id == PROP_BOX_BLUE) {
        g_value_set_uint(value, self->box_blue);
        return;
    }

    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
}

static void fill_background(guint8* data)
{
    for (gint y = 0; y < kHeight; y++) {
        for (gint x = 0; x < kWidth; x++) {
            const gsize offset =
                (static_cast<gsize>(y) * kWidth + x) * kChannels;

            data[offset + 0] = 18;
            data[offset + 1] = 24;
            data[offset + 2] = 32;
        }
    }
}

static gint moving_box_x(guint64 frame_number)
{
    const gint range =
        kWidth - kBoxSize;

    const gint position =
        static_cast<gint>((frame_number * 4) % (range * 2));

    if (position <= range) {
        return position;
    }

    return (range * 2) - position;
}

static gint moving_box_y(guint64 frame_number)
{
    const gint range =
        kHeight - kBoxSize;

    const gint position =
        static_cast<gint>((frame_number * 2) % (range * 2));

    if (position <= range) {
        return position;
    }

    return (range * 2) - position;
}

static void draw_box(
    guint8* data,
    gint box_x,
    gint box_y,
    guint red,
    guint green,
    guint blue)
{
    for (gint y = box_y; y < box_y + kBoxSize; y++) {
        for (gint x = box_x; x < box_x + kBoxSize; x++) {
            const gsize offset =
                (static_cast<gsize>(y) * kWidth + x) * kChannels;

            data[offset + 0] =
                static_cast<guint8>(red);
            data[offset + 1] =
                static_cast<guint8>(green);
            data[offset + 2] =
                static_cast<guint8>(blue);
        }
    }
}

static GstFlowReturn gst_color_box_src_create(
    GstPushSrc* src,
    GstBuffer** buffer)
{
    GstColorBoxSrc* self =
        reinterpret_cast<GstColorBoxSrc*>(src);

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

    fill_background(map_info.data);

    draw_box(
        map_info.data,
        moving_box_x(self->frame_number),
        moving_box_y(self->frame_number),
        self->box_red,
        self->box_green,
        self->box_blue
    );

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

static void gst_color_box_src_get_times(
    GstBaseSrc* src,
    GstBuffer* buffer,
    GstClockTime* start,
    GstClockTime* end)
{
    (void)src;

    *start = GST_CLOCK_TIME_NONE;
    *end = GST_CLOCK_TIME_NONE;

    if (!GST_BUFFER_PTS_IS_VALID(buffer)) {
        return;
    }

    *start = GST_BUFFER_PTS(buffer);

    if (GST_BUFFER_DURATION_IS_VALID(buffer)) {
        *end = *start + GST_BUFFER_DURATION(buffer);
    }
}

static void gst_color_box_src_class_init(
    GstColorBoxSrcClass* klass)
{
    GObjectClass* object_class =
        G_OBJECT_CLASS(klass);

    GstElementClass* element_class =
        GST_ELEMENT_CLASS(klass);

    GstBaseSrcClass* base_src_class =
        GST_BASE_SRC_CLASS(klass);

    GstPushSrcClass* push_src_class =
        GST_PUSH_SRC_CLASS(klass);

    object_class->set_property = gst_color_box_src_set_property;
    object_class->get_property = gst_color_box_src_get_property;

    g_object_class_install_property(
        object_class,
        PROP_BOX_RED,
        g_param_spec_uint(
            "box-red",
            "Box red",
            "Red channel value for the moving box",
            0,
            255,
            255,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
        )
    );

    g_object_class_install_property(
        object_class,
        PROP_BOX_GREEN,
        g_param_spec_uint(
            "box-green",
            "Box green",
            "Green channel value for the moving box",
            0,
            255,
            0,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
        )
    );

    g_object_class_install_property(
        object_class,
        PROP_BOX_BLUE,
        g_param_spec_uint(
            "box-blue",
            "Box blue",
            "Blue channel value for the moving box",
            0,
            255,
            0,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
        )
    );

    gst_element_class_set_static_metadata(
        element_class,
        "ColorBoxSrc",
        "Source/Video",
        "Generates RGB video with a moving configurable-color box",
        "example"
    );

    gst_element_class_add_static_pad_template(
        element_class,
        &src_template
    );

    base_src_class->get_times =
        GST_DEBUG_FUNCPTR(gst_color_box_src_get_times);

    push_src_class->create =
        GST_DEBUG_FUNCPTR(gst_color_box_src_create);
}

static void gst_color_box_src_init(
    GstColorBoxSrc* self)
{
    self->frame_number = 0;
    self->box_red = 255;
    self->box_green = 0;
    self->box_blue = 0;

    gst_base_src_set_format(
        GST_BASE_SRC(self),
        GST_FORMAT_TIME
    );

    gst_base_src_set_live(
        GST_BASE_SRC(self),
        TRUE
    );
}

static gboolean plugin_init(GstPlugin* plugin)
{
    return gst_element_register(
        plugin,
        "colorboxsrc",
        GST_RANK_NONE,
        GST_TYPE_COLOR_BOX_SRC
    );
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    colorboxsrc,
    "Moving configurable-color box video source",
    plugin_init,
    "1.0",
    "LGPL",
    "colorboxsrc",
    "https://example.com"
)
