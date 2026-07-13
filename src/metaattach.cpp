#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

#ifndef PACKAGE
#define PACKAGE "metaattach"
#endif

static constexpr const char* kMetaName = "GstTutorialMeta";

typedef struct _GstMetaAttach {
    GstBaseTransform parent;
    guint64 frame_count;
} GstMetaAttach;

typedef struct _GstMetaAttachClass {
    GstBaseTransformClass parent_class;
} GstMetaAttachClass;

#define GST_TYPE_META_ATTACH (gst_meta_attach_get_type())

G_DEFINE_TYPE(
    GstMetaAttach,
    gst_meta_attach,
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

static GstFlowReturn gst_meta_attach_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
{
    GstMetaAttach* self =
        reinterpret_cast<GstMetaAttach*>(base);

    ensure_tutorial_meta_registered();

    GstCustomMeta* meta =
        gst_buffer_add_custom_meta(buffer, kMetaName);

    if (meta == nullptr) {
        GST_WARNING_OBJECT(base, "failed to attach %s", kMetaName);
        return GST_FLOW_OK;
    }

    const GstClockTime pts = GST_BUFFER_PTS(buffer);
    GstStructure* structure =
        gst_custom_meta_get_structure(meta);

    gst_structure_set(
        structure,
        "message", G_TYPE_STRING, "hello from metaattach",
        "frame-number", G_TYPE_UINT64, self->frame_count,
        "pts-valid", G_TYPE_BOOLEAN, GST_CLOCK_TIME_IS_VALID(pts),
        "pts", G_TYPE_UINT64, GST_CLOCK_TIME_IS_VALID(pts) ? pts : 0,
        NULL
    );

    self->frame_count++;

    return GST_FLOW_OK;
}

static void gst_meta_attach_class_init(
    GstMetaAttachClass* klass)
{
    GstElementClass* element_class =
        GST_ELEMENT_CLASS(klass);

    GstBaseTransformClass* transform_class =
        GST_BASE_TRANSFORM_CLASS(klass);

    gst_element_class_set_static_metadata(
        element_class,
        "MetaAttach",
        "Filter/Metadata",
        "Attaches simple custom metadata to each buffer",
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
        GST_DEBUG_FUNCPTR(gst_meta_attach_transform_ip);
}

static void gst_meta_attach_init(
    GstMetaAttach* self)
{
    self->frame_count = 0;

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

    return gst_element_register(
        plugin,
        "metaattach",
        GST_RANK_NONE,
        GST_TYPE_META_ATTACH
    );
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    metaattach,
    "Plugin that attaches custom buffer metadata",
    plugin_init,
    "1.0",
    "LGPL",
    "metaattach",
    "https://example.com"
)
