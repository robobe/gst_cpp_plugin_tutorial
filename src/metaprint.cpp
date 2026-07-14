#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

#ifndef PACKAGE
#define PACKAGE "metaprint"
#endif

static constexpr const char* kMetaName = "GstTutorialMeta";
static constexpr const char* kDetectionMetaName = "GstRedDetectionMeta";

typedef struct _GstMetaPrint {
    GstBaseTransform parent;
} GstMetaPrint;

typedef struct _GstMetaPrintClass {
    GstBaseTransformClass parent_class;
} GstMetaPrintClass;

#define GST_TYPE_META_PRINT (gst_meta_print_get_type())

G_DEFINE_TYPE(
    GstMetaPrint,
    gst_meta_print,
    GST_TYPE_BASE_TRANSFORM
)

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE(
        "sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-raw")
    );

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-raw")
    );

static void ensure_tutorial_meta_registered()
{
    if (gst_meta_get_info(kMetaName) == nullptr) {
        gst_meta_register_custom_simple(kMetaName);
    }
}

static void ensure_detection_meta_registered()
{
    if (gst_meta_get_info(kDetectionMetaName) == nullptr) {
        gst_meta_register_custom_simple(kDetectionMetaName);
    }
}

static gboolean print_detection_meta(GstBuffer* buffer)
{
    ensure_detection_meta_registered();

    GstCustomMeta* meta =
        gst_buffer_get_custom_meta(buffer, kDetectionMetaName);

    if (meta == nullptr) {
        return FALSE;
    }

    GstStructure* structure =
        gst_custom_meta_get_structure(meta);

    gboolean found = FALSE;
    gint x = 0;
    gint y = 0;
    gint width = 0;
    gint height = 0;

    gst_structure_get_boolean(
        structure,
        "found",
        &found
    );

    gst_structure_get_int(
        structure,
        "x",
        &x
    );

    gst_structure_get_int(
        structure,
        "y",
        &y
    );

    gst_structure_get_int(
        structure,
        "width",
        &width
    );

    gst_structure_get_int(
        structure,
        "height",
        &height
    );

    g_print(
        "metaprint: found=%s x=%d y=%d width=%d height=%d\n",
        found ? "true" : "false",
        x,
        y,
        width,
        height
    );

    return TRUE;
}

static GstFlowReturn gst_meta_print_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
{
    if (print_detection_meta(buffer)) {
        return GST_FLOW_OK;
    }

    ensure_tutorial_meta_registered();

    GstCustomMeta* meta =
        gst_buffer_get_custom_meta(buffer, kMetaName);

    if (meta == nullptr) {
        g_print("metaprint: no %s found\n", kMetaName);
        return GST_FLOW_OK;
    }

    GstStructure* structure =
        gst_custom_meta_get_structure(meta);

    const gchar* message =
        gst_structure_get_string(structure, "message");

    guint64 frame_number = 0;
    gboolean pts_valid = FALSE;
    guint64 pts = 0;

    gst_structure_get_uint64(
        structure,
        "frame-number",
        &frame_number
    );

    gst_structure_get_boolean(
        structure,
        "pts-valid",
        &pts_valid
    );

    gst_structure_get_uint64(
        structure,
        "pts",
        &pts
    );

    if (pts_valid) {
        g_print(
            "metaprint: message=\"%s\" frame=%" G_GUINT64_FORMAT " pts=%" GST_TIME_FORMAT "\n",
            message != nullptr ? message : "",
            frame_number,
            GST_TIME_ARGS(pts)
        );
    } else {
        g_print(
            "metaprint: message=\"%s\" frame=%" G_GUINT64_FORMAT " pts=GST_CLOCK_TIME_NONE\n",
            message != nullptr ? message : "",
            frame_number
        );
    }

    (void)base;

    return GST_FLOW_OK;
}

static void gst_meta_print_class_init(
    GstMetaPrintClass* klass)
{
    GstElementClass* element_class =
        GST_ELEMENT_CLASS(klass);

    GstBaseTransformClass* transform_class =
        GST_BASE_TRANSFORM_CLASS(klass);

    gst_element_class_set_static_metadata(
        element_class,
        "MetaPrint",
        "Filter/Metadata",
        "Reads custom buffer metadata and prints it",
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

    transform_class->transform_ip =
        GST_DEBUG_FUNCPTR(gst_meta_print_transform_ip);
}

static void gst_meta_print_init(
    GstMetaPrint* self)
{
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
    ensure_tutorial_meta_registered();
    ensure_detection_meta_registered();

    return gst_element_register(
        plugin,
        "metaprint",
        GST_RANK_NONE,
        GST_TYPE_META_PRINT
    );
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    metaprint,
    "Plugin that reads and prints custom buffer metadata",
    plugin_init,
    "1.0",
    "LGPL",
    "metaprint",
    "https://example.com"
)
