#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

#ifndef PACKAGE
#define PACKAGE "propertyfilter"
#endif

typedef struct _GstPropertyFilter {
    GstBaseTransform parent;
    gboolean print_pts;
} GstPropertyFilter;

typedef struct _GstPropertyFilterClass {
    GstBaseTransformClass parent_class;
} GstPropertyFilterClass;

#define GST_TYPE_PROPERTY_FILTER (gst_property_filter_get_type())

G_DEFINE_TYPE(
    GstPropertyFilter,
    gst_property_filter,
    GST_TYPE_BASE_TRANSFORM
)

enum {
    PROP_0,
    PROP_PRINT_PTS
};

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

static void gst_property_filter_set_property(
    GObject* object,
    guint property_id,
    const GValue* value,
    GParamSpec* pspec)
{
    GstPropertyFilter* self =
        reinterpret_cast<GstPropertyFilter*>(object);

    if (property_id == PROP_PRINT_PTS) {
        self->print_pts = g_value_get_boolean(value);
        return;
    }

    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
}

static void gst_property_filter_get_property(
    GObject* object,
    guint property_id,
    GValue* value,
    GParamSpec* pspec)
{
    GstPropertyFilter* self =
        reinterpret_cast<GstPropertyFilter*>(object);

    if (property_id == PROP_PRINT_PTS) {
        g_value_set_boolean(value, self->print_pts);
        return;
    }

    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
}

static GstFlowReturn gst_property_filter_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
{
    GstPropertyFilter* self =
        reinterpret_cast<GstPropertyFilter*>(base);

    if (!self->print_pts) {
        return GST_FLOW_OK;
    }

    const GstClockTime pts = GST_BUFFER_PTS(buffer);

    if (GST_CLOCK_TIME_IS_VALID(pts)) {
        g_print(
            "propertyfilter pts: %" GST_TIME_FORMAT "\n",
            GST_TIME_ARGS(pts)
        );
    } else {
        g_print("propertyfilter pts: GST_CLOCK_TIME_NONE\n");
    }

    return GST_FLOW_OK;
}

static void gst_property_filter_class_init(
    GstPropertyFilterClass* klass)
{
    GObjectClass* object_class =
        G_OBJECT_CLASS(klass);

    GstElementClass* element_class =
        GST_ELEMENT_CLASS(klass);

    GstBaseTransformClass* transform_class =
        GST_BASE_TRANSFORM_CLASS(klass);

    object_class->set_property = gst_property_filter_set_property;
    object_class->get_property = gst_property_filter_get_property;

    g_object_class_install_property(
        object_class,
        PROP_PRINT_PTS,
        g_param_spec_boolean(
            "print-pts",
            "Print PTS",
            "Print the presentation timestamp for each buffer",
            TRUE,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
        )
    );

    gst_element_class_set_static_metadata(
        element_class,
        "PropertyFilter",
        "Filter/Video",
        "C++ GstBaseTransform plugin with a boolean property",
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
        GST_DEBUG_FUNCPTR(gst_property_filter_transform_ip);
}

static void gst_property_filter_init(
    GstPropertyFilter* self)
{
    self->print_pts = TRUE;

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
        "propertyfilter",
        GST_RANK_NONE,
        GST_TYPE_PROPERTY_FILTER
    );
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    propertyfilter,
    "GstBaseTransform plugin with a property",
    plugin_init,
    "1.0",
    "LGPL",
    "propertyfilter",
    "https://example.com"
)
