#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

#ifndef PACKAGE
#define PACKAGE "minimalfilter"
#endif

#pragma region Type declarations

// GStreamer elements are GObject types written in C style, even when the plugin
// source file is compiled as C++. To create a new element, we define two structs:
//
// - GstMinimalFilter: one object instance in a running pipeline.
// - GstMinimalFilterClass: shared class data and virtual methods for the type.
//
// Both structs place the parent type as the first field. This is how GObject
// implements inheritance in C: a pointer to GstMinimalFilter can be safely cast
// to GstBaseTransform because the parent struct starts at the same address.

typedef struct _GstMinimalFilter {
    // Instance parent. Add per-element runtime state after this field if the
    // filter needs properties, counters, cached caps, or other object data.
    GstBaseTransform parent;
} GstMinimalFilter;

typedef struct _GstMinimalFilterClass {
    // Class parent. Virtual methods from GstBaseTransformClass are configured in
    // gst_minimal_filter_class_init().
    GstBaseTransformClass parent_class;
} GstMinimalFilterClass;

// Convenience macro for the GType ID of this element. GStreamer uses GType IDs
// to know which class to instantiate and how it relates to parent classes.
#define GST_TYPE_MINIMAL_FILTER (gst_minimal_filter_get_type())

// Registers GstMinimalFilter as a GObject type derived from GstBaseTransform.
//
// Arguments:
// - GstMinimalFilter: C type name of the instance struct.
// - gst_minimal_filter: function-name prefix for generated GObject glue.
// - GST_TYPE_BASE_TRANSFORM: parent GObject type.
//
// The macro generates gst_minimal_filter_get_type(), which returns the GType ID
// used by GST_TYPE_MINIMAL_FILTER. It also wires the two convention-based
// functions implemented later in this file:
//
// - gst_minimal_filter_class_init()
// - gst_minimal_filter_init()
G_DEFINE_TYPE(
    GstMinimalFilter,
    gst_minimal_filter,
    GST_TYPE_BASE_TRANSFORM
)

#pragma endregion

#pragma region Pad templates

// Pad templates describe the static pads exposed by the element. This filter has
// one always-present video sink pad and one always-present video source pad.

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

#pragma endregion

#pragma region Buffer processing

// transform_ip() is the in-place processing callback from GstBaseTransform. It
// receives each buffer, logs its presentation timestamp, and returns it unchanged.

static GstFlowReturn gst_minimal_filter_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
{
    // The base pointer is the current element instance. This minimal example
    // does not need instance state, so mark it as intentionally unused.
    (void)base;

    // PTS means presentation timestamp: the stream time when this buffer should
    // be presented downstream. Some buffers may not carry a valid timestamp.
    const GstClockTime pts = GST_BUFFER_PTS(buffer);

    if (GST_CLOCK_TIME_IS_VALID(pts)) {
        // GST_TIME_FORMAT and GST_TIME_ARGS print GstClockTime values in a
        // readable hours/minutes/seconds/nanoseconds form.
        g_print(
            "buffer pts: %" GST_TIME_FORMAT "\n",
            GST_TIME_ARGS(pts)
        );
    } else {
        // GST_CLOCK_TIME_NONE is the common value for missing timestamps.
        g_print("buffer pts: GST_CLOCK_TIME_NONE\n");
    }

    // Returning GST_FLOW_OK tells GStreamer processing succeeded. Because this
    // is an in-place transform and the buffer was not edited, it is forwarded
    // downstream unchanged.
    return GST_FLOW_OK;
}

#pragma endregion

#pragma region Class initialization

// Class initialization runs once for the element type. It publishes element
// metadata, declares pad templates, and connects virtual methods.

static void gst_minimal_filter_class_init(
    GstMinimalFilterClass* klass)
{
    GstElementClass* element_class =
        GST_ELEMENT_CLASS(klass);

    GstBaseTransformClass* transform_class =
        GST_BASE_TRANSFORM_CLASS(klass);

    gst_element_class_set_static_metadata(
        element_class,
        "MinimalFilter",
        "Filter/Video",
        "Minimal C++ GStreamer passthrough element",
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
        GST_DEBUG_FUNCPTR(gst_minimal_filter_transform_ip);
}

#pragma endregion

#pragma region Instance initialization

// Instance initialization runs once for every new minimalfilter element object.
//
// GStreamer knows about this function through the earlier G_DEFINE_TYPE call:
//
//     G_DEFINE_TYPE(GstMinimalFilter, gst_minimal_filter, GST_TYPE_BASE_TRANSFORM)
//
// The second argument, gst_minimal_filter, is the type-name prefix. The macro
// expects two functions with that prefix:
//
// - gst_minimal_filter_class_init(): initializes the element class once.
// - gst_minimal_filter_init(): initializes each element instance.
//
// When a pipeline creates the element factory named "minimalfilter", GStreamer
// asks the GObject type system to instantiate GST_TYPE_MINIMAL_FILTER. During
// that object creation, the type system calls gst_minimal_filter_init(self).
//
// These settings make GstBaseTransform call transform_ip() for each input
// buffer instead of treating the element as a pure passthrough.

static void gst_minimal_filter_init(
    GstMinimalFilter* self)
{
    // self is the newly created element instance. The GST_BASE_TRANSFORM() macro
    // casts it from this element type to its GstBaseTransform parent type.

    // Tell GstBaseTransform that processing happens on the input buffer itself.
    gst_base_transform_set_in_place(
        GST_BASE_TRANSFORM(self),
        TRUE
    );

    // Keep passthrough mode disabled so transform_ip() is invoked.
    gst_base_transform_set_passthrough(
        GST_BASE_TRANSFORM(self),
        FALSE
    );
}

#pragma endregion

#pragma region Plugin registration

// plugin_init() is the plugin entry point called by GStreamer after it finds
// and opens this shared library. The function receives the GstPlugin object that
// represents this loaded .so file inside the GStreamer registry.

static gboolean plugin_init(GstPlugin* plugin)
{
    // Register this element type inside the plugin.
    //
    // "minimalfilter" is the factory name. This is the name users put in a
    // pipeline and the name inspected by:
    //
    //     gst-inspect-1.0 minimalfilter
    //
    // GST_RANK_NONE means this element is not auto-selected by GStreamer when it
    // chooses plugins automatically. That is fine for a custom explicit filter.
    //
    // GST_TYPE_MINIMAL_FILTER is the GObject type created earlier by
    // G_DEFINE_TYPE. It tells GStreamer which element class to instantiate when
    // a pipeline requests "minimalfilter".
    return gst_element_register(
        plugin,
        "minimalfilter",
        GST_RANK_NONE,
        GST_TYPE_MINIMAL_FILTER
    );
}

// GST_PLUGIN_DEFINE exports a C symbol that GStreamer's plugin scanner expects.
// The rest of the file is C++, so extern "C" prevents C++ name mangling for that
// exported plugin descriptor.
extern "C" {

// This macro provides the static metadata for the plugin file itself, not for
// the element. GStreamer reads this descriptor while scanning
// libgstminimalfilter.so and then calls plugin_init() above.
//
// Arguments:
// - GST_VERSION_MAJOR / GST_VERSION_MINOR: GStreamer ABI version expected.
// - minimalfilter: plugin name stored in the registry.
// - description: human-readable plugin description.
// - plugin_init: function that registers the element factory.
// - "1.0": plugin version.
// - "LGPL": plugin license string.
// - "minimalfilter": source/package name.
// - "https://example.com": package origin URL.
GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    minimalfilter,
    "Minimal C++ GstBaseTransform plugin",
    plugin_init,
    "1.0",
    "LGPL",
    "minimalfilter",
    "https://example.com"
)

}

#pragma endregion