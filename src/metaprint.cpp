#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>

#ifndef PACKAGE
#define PACKAGE "metaprint"
#endif

static constexpr const char* kMetaName = "GstTutorialMeta";
static constexpr const char* kDetectionMetaName = "GstRedDetectionMeta";

static void register_custom_meta(const char* name)
{
#if GST_CHECK_VERSION(1, 24, 0)
    gst_meta_register_custom_simple(name);
#else
    const gchar* tags[] = {nullptr};
    gst_meta_register_custom(name, tags, nullptr, nullptr, nullptr);
#endif
}

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
        register_custom_meta(kMetaName);
    }
}

static void ensure_detection_meta_registered()
{
    if (gst_meta_get_info(kDetectionMetaName) == nullptr) {
        register_custom_meta(kDetectionMetaName);
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

static gboolean print_roi_meta(GstBuffer* buffer)
{
    gpointer state = nullptr;
    gboolean found = FALSE;
    GstMeta* meta = nullptr;
    while ((meta = gst_buffer_iterate_meta_filtered(
                buffer,
                &state,
                GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)) != nullptr) {
        GstVideoRegionOfInterestMeta* roi =
            reinterpret_cast<GstVideoRegionOfInterestMeta*>(meta);
        GstStructure* parameters =
            gst_video_region_of_interest_meta_get_param(roi, "yolo");
        if (parameters == nullptr) {
            parameters = gst_video_region_of_interest_meta_get_param(roi, "nanotrack");
            if (parameters == nullptr) continue;
            gboolean initialized = FALSE;
            gdouble confidence = 0;
            gst_structure_get_boolean(parameters, "initialized", &initialized);
            const gboolean has_confidence = gst_structure_get_double(parameters, "confidence", &confidence);
            g_print("metaprint: roi=%s initialized=%s x=%u y=%u width=%u height=%u",
                    g_quark_to_string(roi->roi_type), initialized ? "true" : "false",
                    roi->x, roi->y, roi->w, roi->h);
            if (has_confidence) g_print(" confidence=%.6f", confidence);
            const GstClockTime pts = GST_BUFFER_PTS(buffer);
            if (GST_CLOCK_TIME_IS_VALID(pts))
                g_print(" pts=%" GST_TIME_FORMAT "\n", GST_TIME_ARGS(pts));
            else
                g_print(" pts=GST_CLOCK_TIME_NONE\n");
            found = TRUE;
            continue;
        }

        gint class_id = 0;
        gdouble confidence = 0.0;
        gst_structure_get_int(parameters, "class-id", &class_id);
        gst_structure_get_double(parameters, "confidence", &confidence);

        const GstClockTime pts = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(pts)) {
            g_print(
                "metaprint: roi=%s class=%d confidence=%.6f x=%u y=%u width=%u height=%u pts=%" GST_TIME_FORMAT "\n",
                g_quark_to_string(roi->roi_type),
                class_id,
                confidence,
                roi->x,
                roi->y,
                roi->w,
                roi->h,
                GST_TIME_ARGS(pts)
            );
        } else {
            g_print(
                "metaprint: roi=%s class=%d confidence=%.6f x=%u y=%u width=%u height=%u pts=GST_CLOCK_TIME_NONE\n",
                g_quark_to_string(roi->roi_type),
                class_id,
                confidence,
                roi->x,
                roi->y,
                roi->w,
                roi->h
            );
        }
        found = TRUE;
    }
    return found;
}

static GstFlowReturn gst_meta_print_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
{
    if (print_roi_meta(buffer)) {
        return GST_FLOW_OK;
    }

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
        "Reads ROI or custom buffer metadata and prints it",
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
    "Plugin that reads and prints ROI or custom buffer metadata",
    plugin_init,
    "1.0",
    "LGPL",
    "metaprint",
    "https://example.com"
)
