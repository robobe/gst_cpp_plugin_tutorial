#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <vector>

#ifndef PACKAGE
#define PACKAGE "reddetect"
#endif

GST_DEBUG_CATEGORY_STATIC(gst_red_detect_debug);
#define GST_CAT_DEFAULT gst_red_detect_debug

static constexpr const char* kDetectionMetaName = "GstRedDetectionMeta";

typedef struct _GstRedDetect {
    GstBaseTransform parent;
    GstVideoInfo video_info;
    guint low_h;
    guint low_s;
    guint low_v;
    guint high_h;
    guint high_s;
    guint high_v;
} GstRedDetect;

typedef struct _GstRedDetectClass {
    GstBaseTransformClass parent_class;
} GstRedDetectClass;

#define GST_TYPE_RED_DETECT (gst_red_detect_get_type())

G_DEFINE_TYPE(
    GstRedDetect,
    gst_red_detect,
    GST_TYPE_BASE_TRANSFORM
)

enum {
    PROP_0,
    PROP_LOW_H,
    PROP_LOW_S,
    PROP_LOW_V,
    PROP_HIGH_H,
    PROP_HIGH_S,
    PROP_HIGH_V
};

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

static void ensure_detection_meta_registered()
{
    if (gst_meta_get_info(kDetectionMetaName) == nullptr) {
        gst_meta_register_custom_simple(kDetectionMetaName);
    }
}

static void gst_red_detect_set_property(
    GObject* object,
    guint property_id,
    const GValue* value,
    GParamSpec* pspec)
{
    GstRedDetect* self =
        reinterpret_cast<GstRedDetect*>(object);

    if (property_id == PROP_LOW_H) {
        self->low_h = g_value_get_uint(value);
        return;
    }

    if (property_id == PROP_LOW_S) {
        self->low_s = g_value_get_uint(value);
        return;
    }

    if (property_id == PROP_LOW_V) {
        self->low_v = g_value_get_uint(value);
        return;
    }

    if (property_id == PROP_HIGH_H) {
        self->high_h = g_value_get_uint(value);
        return;
    }

    if (property_id == PROP_HIGH_S) {
        self->high_s = g_value_get_uint(value);
        return;
    }

    if (property_id == PROP_HIGH_V) {
        self->high_v = g_value_get_uint(value);
        return;
    }

    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
}

static void gst_red_detect_get_property(
    GObject* object,
    guint property_id,
    GValue* value,
    GParamSpec* pspec)
{
    GstRedDetect* self =
        reinterpret_cast<GstRedDetect*>(object);

    if (property_id == PROP_LOW_H) {
        g_value_set_uint(value, self->low_h);
        return;
    }

    if (property_id == PROP_LOW_S) {
        g_value_set_uint(value, self->low_s);
        return;
    }

    if (property_id == PROP_LOW_V) {
        g_value_set_uint(value, self->low_v);
        return;
    }

    if (property_id == PROP_HIGH_H) {
        g_value_set_uint(value, self->high_h);
        return;
    }

    if (property_id == PROP_HIGH_S) {
        g_value_set_uint(value, self->high_s);
        return;
    }

    if (property_id == PROP_HIGH_V) {
        g_value_set_uint(value, self->high_v);
        return;
    }

    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
}

static gboolean gst_red_detect_set_caps(
    GstBaseTransform* base,
    GstCaps* input_caps,
    GstCaps* output_caps)
{
    (void)output_caps;

    GstRedDetect* self =
        reinterpret_cast<GstRedDetect*>(base);

    return gst_video_info_from_caps(
        &self->video_info,
        input_caps
    );
}

static GstFlowReturn gst_red_detect_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
{
    GstRedDetect* self =
        reinterpret_cast<GstRedDetect*>(base);

    GstVideoFrame video_frame;
    if (!gst_video_frame_map(
            &video_frame,
            &self->video_info,
            buffer,
            GST_MAP_READ)) {
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

    cv::Mat hsv;
    cv::cvtColor(
        rgb,
        hsv,
        cv::COLOR_RGB2HSV
    );

    cv::Mat mask;
    cv::inRange(
        hsv,
        cv::Scalar(self->low_h, self->low_s, self->low_v),
        cv::Scalar(self->high_h, self->high_s, self->high_v),
        mask
    );

    std::vector<cv::Point> red_pixels;
    cv::findNonZero(mask, red_pixels);

    gboolean found = FALSE;
    gint box_x = 0;
    gint box_y = 0;
    gint box_width = 0;
    gint box_height = 0;

    if (red_pixels.empty()) {
        GST_LOG_OBJECT(base, "red box not found");
    } else {
        const cv::Rect box =
            cv::boundingRect(red_pixels);

        found = TRUE;
        box_x = box.x;
        box_y = box.y;
        box_width = box.width;
        box_height = box.height;

        GST_LOG_OBJECT(
            base,
            "red box found x=%d y=%d width=%d height=%d",
            box_x,
            box_y,
            box_width,
            box_height
        );
    }

    gst_video_frame_unmap(&video_frame);

    ensure_detection_meta_registered();

    GstCustomMeta* meta =
        gst_buffer_add_custom_meta(buffer, kDetectionMetaName);

    if (meta == nullptr) {
        GST_WARNING_OBJECT(base, "failed to attach %s", kDetectionMetaName);
        return GST_FLOW_OK;
    }

    GstStructure* structure =
        gst_custom_meta_get_structure(meta);

    gst_structure_set(
        structure,
        "found", G_TYPE_BOOLEAN, found,
        "x", G_TYPE_INT, box_x,
        "y", G_TYPE_INT, box_y,
        "width", G_TYPE_INT, box_width,
        "height", G_TYPE_INT, box_height,
        NULL
    );

    return GST_FLOW_OK;
}

static void install_uint_property(
    GObjectClass* object_class,
    guint property_id,
    const gchar* name,
    const gchar* nick,
    const gchar* blurb,
    guint maximum,
    guint default_value)
{
    g_object_class_install_property(
        object_class,
        property_id,
        g_param_spec_uint(
            name,
            nick,
            blurb,
            0,
            maximum,
            default_value,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
        )
    );
}

static void gst_red_detect_class_init(
    GstRedDetectClass* klass)
{
    GObjectClass* object_class =
        G_OBJECT_CLASS(klass);

    GstElementClass* element_class =
        GST_ELEMENT_CLASS(klass);

    GstBaseTransformClass* transform_class =
        GST_BASE_TRANSFORM_CLASS(klass);

    object_class->set_property = gst_red_detect_set_property;
    object_class->get_property = gst_red_detect_get_property;

    install_uint_property(
        object_class,
        PROP_LOW_H,
        "low-h",
        "Low hue",
        "Lower HSV hue threshold",
        179,
        0
    );

    install_uint_property(
        object_class,
        PROP_LOW_S,
        "low-s",
        "Low saturation",
        "Lower HSV saturation threshold",
        255,
        100
    );

    install_uint_property(
        object_class,
        PROP_LOW_V,
        "low-v",
        "Low value",
        "Lower HSV value threshold",
        255,
        100
    );

    install_uint_property(
        object_class,
        PROP_HIGH_H,
        "high-h",
        "High hue",
        "Upper HSV hue threshold",
        179,
        10
    );

    install_uint_property(
        object_class,
        PROP_HIGH_S,
        "high-s",
        "High saturation",
        "Upper HSV saturation threshold",
        255,
        255
    );

    install_uint_property(
        object_class,
        PROP_HIGH_V,
        "high-v",
        "High value",
        "Upper HSV value threshold",
        255,
        255
    );

    gst_element_class_set_static_metadata(
        element_class,
        "RedDetect",
        "Filter/Video",
        "Detects red pixels in RGB video buffers with OpenCV HSV thresholding",
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
        GST_DEBUG_FUNCPTR(gst_red_detect_set_caps);

    transform_class->transform_ip =
        GST_DEBUG_FUNCPTR(gst_red_detect_transform_ip);
}

static void gst_red_detect_init(
    GstRedDetect* self)
{
    gst_video_info_init(&self->video_info);

    self->low_h = 0;
    self->low_s = 100;
    self->low_v = 100;
    self->high_h = 10;
    self->high_s = 255;
    self->high_v = 255;

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
    ensure_detection_meta_registered();

    GST_DEBUG_CATEGORY_INIT(
        gst_red_detect_debug,
        "reddetect",
        0,
        "OpenCV red object detection filter"
    );

    return gst_element_register(
        plugin,
        "reddetect",
        GST_RANK_NONE,
        GST_TYPE_RED_DETECT
    );
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    reddetect,
    "OpenCV red object detection filter",
    plugin_init,
    "1.0",
    "LGPL",
    "reddetect",
    "https://example.com"
)
