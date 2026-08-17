#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

#pragma region Boilerplate: GObject type and pad declaration

#ifndef PACKAGE
#define PACKAGE "islivesrc"
#endif

static constexpr gint kFps = 30;

typedef struct _GstIsLiveSrc {
    GstPushSrc parent;
    guint64 frame_number;
    gboolean is_live;
} GstIsLiveSrc;

typedef struct _GstIsLiveSrcClass {
    GstPushSrcClass parent_class;
} GstIsLiveSrcClass;

#define GST_TYPE_IS_LIVE_SRC (gst_is_live_src_get_type())

G_DEFINE_TYPE(
    GstIsLiveSrc,
    gst_is_live_src,
    GST_TYPE_PUSH_SRC
)

enum {
    PROP_0,
    PROP_IS_LIVE
};

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS("application/x-is-live-example")
    );

#pragma endregion

#pragma region Important: live versus non-live behavior

static void gst_is_live_src_set_property(
    GObject* object,
    guint property_id,
    const GValue* value,
    GParamSpec* pspec)
{
    GstIsLiveSrc* self =
        reinterpret_cast<GstIsLiveSrc*>(object);

    if (property_id == PROP_IS_LIVE) {
        self->is_live = g_value_get_boolean(value);

        // TRUE:  create() runs only in PLAYING and READY -> PAUSED reports
        //        GST_STATE_CHANGE_NO_PREROLL.
        // FALSE: create() may run in PAUSED to provide a preroll buffer.
        gst_base_src_set_live(
            GST_BASE_SRC(self),
            self->is_live
        );
        return;
    }

    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
}

static void gst_is_live_src_get_times(
    GstBaseSrc* src,
    GstBuffer* buffer,
    GstClockTime* start,
    GstClockTime* end)
{
    GstIsLiveSrc* self =
        reinterpret_cast<GstIsLiveSrc*>(src);

    *start = GST_CLOCK_TIME_NONE;
    *end = GST_CLOCK_TIME_NONE;

    // Returning no times means GstBaseSrc does not wait on the clock. A
    // non-live source therefore pushes these buffers as fast as downstream
    // accepts them. The is-live flag by itself does not provide 30 fps pacing.
    if (!self->is_live || !GST_BUFFER_PTS_IS_VALID(buffer)) {
        return;
    }

    // A live pseudo-source returns its stream times. GstBaseSrc converts them
    // to pipeline running time and waits before pushing the buffer downstream.
    *start = GST_BUFFER_PTS(buffer);

    if (GST_BUFFER_DURATION_IS_VALID(buffer)) {
        *end = *start + GST_BUFFER_DURATION(buffer);
    }
}

#pragma endregion

#pragma region Important: create one empty 30 fps buffer

static GstFlowReturn gst_is_live_src_create(
    GstPushSrc* src,
    GstBuffer** buffer)
{
    GstIsLiveSrc* self =
        reinterpret_cast<GstIsLiveSrc*>(src);

    GstBuffer* out_buffer = gst_buffer_new();
    if (out_buffer == nullptr) {
        return GST_FLOW_ERROR;
    }

    const GstClockTime pts = gst_util_uint64_scale(
        self->frame_number,
        GST_SECOND,
        kFps
    );

    const GstClockTime next_pts = gst_util_uint64_scale(
        self->frame_number + 1,
        GST_SECOND,
        kFps
    );

    GST_BUFFER_PTS(out_buffer) = pts;
    GST_BUFFER_DTS(out_buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(out_buffer) = next_pts - pts;
    GST_BUFFER_OFFSET(out_buffer) = self->frame_number;

    self->frame_number++;
    *buffer = out_buffer;

    return GST_FLOW_OK;
}

#pragma endregion

#pragma region Boilerplate: properties, lifecycle, and registration

static void gst_is_live_src_get_property(
    GObject* object,
    guint property_id,
    GValue* value,
    GParamSpec* pspec)
{
    GstIsLiveSrc* self =
        reinterpret_cast<GstIsLiveSrc*>(object);

    if (property_id == PROP_IS_LIVE) {
        g_value_set_boolean(value, self->is_live);
        return;
    }

    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
}

static gboolean gst_is_live_src_start(GstBaseSrc* src)
{
    GstIsLiveSrc* self =
        reinterpret_cast<GstIsLiveSrc*>(src);

    self->frame_number = 0;
    return TRUE;
}

static void gst_is_live_src_class_init(GstIsLiveSrcClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
    GstBaseSrcClass* base_src_class = GST_BASE_SRC_CLASS(klass);
    GstPushSrcClass* push_src_class = GST_PUSH_SRC_CLASS(klass);

    object_class->set_property = gst_is_live_src_set_property;
    object_class->get_property = gst_is_live_src_get_property;

    g_object_class_install_property(
        object_class,
        PROP_IS_LIVE,
        g_param_spec_boolean(
            "is-live",
            "Is live",
            "Whether the source behaves as a live source",
            TRUE,
            static_cast<GParamFlags>(
                G_PARAM_READWRITE |
                G_PARAM_STATIC_STRINGS |
                GST_PARAM_MUTABLE_READY
            )
        )
    );

    gst_element_class_set_static_metadata(
        element_class,
        "IsLiveSrc",
        "Source",
        "Demonstrates live source state and clock behavior",
        "example"
    );

    gst_element_class_add_static_pad_template(
        element_class,
        &src_template
    );

    // These three callbacks connect the important code above to GstBaseSrc.
    base_src_class->start =
        GST_DEBUG_FUNCPTR(gst_is_live_src_start);

    base_src_class->get_times =
        GST_DEBUG_FUNCPTR(gst_is_live_src_get_times);

    push_src_class->create =
        GST_DEBUG_FUNCPTR(gst_is_live_src_create);
}

static void gst_is_live_src_init(GstIsLiveSrc* self)
{
    self->frame_number = 0;
    self->is_live = TRUE;

    gst_base_src_set_format(
        GST_BASE_SRC(self),
        GST_FORMAT_TIME
    );

    gst_base_src_set_live(
        GST_BASE_SRC(self),
        self->is_live
    );
}

static gboolean plugin_init(GstPlugin* plugin)
{
    return gst_element_register(
        plugin,
        "is_livesrc",
        GST_RANK_NONE,
        GST_TYPE_IS_LIVE_SRC
    );
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    islivesrc,
    "Minimal live source behavior example",
    plugin_init,
    "1.0",
    "LGPL",
    "islivesrc",
    "https://example.com"
)

#pragma endregion
