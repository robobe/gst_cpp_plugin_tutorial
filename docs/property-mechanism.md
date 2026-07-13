# GStreamer Property Mechanism

This page explains how the `propertyfilter` example adds a property to a
GStreamer element.

## What A Property Is

GStreamer elements are GObject objects. GObject has a built-in property system:
an element can expose named values that applications and `gst-launch-1.0` can
set from the outside.

In this project, `propertyfilter` exposes one property:

```text
print-pts
```

When `print-pts=true`, the element prints the buffer timestamp. When
`print-pts=false`, the element forwards buffers silently.

## Runtime Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 videotestsrc num-buffers=3 ! video/x-raw ! propertyfilter print-pts=true ! fakesink
```

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 videotestsrc num-buffers=3 ! video/x-raw ! propertyfilter print-pts=false ! fakesink
```

The syntax is:

```text
element-name property-name=value
```

## Where The Value Is Stored

The instance struct has a field for the property value:

```cpp
typedef struct _GstPropertyFilter {
    GstBaseTransform parent;
    gboolean print_pts;
} GstPropertyFilter;
```

Each element instance gets its own `print_pts` value.

## Property IDs

The code gives each property an integer ID:

```cpp
enum {
    PROP_0,
    PROP_PRINT_PTS
};
```

`PROP_0` is kept unused by GObject convention. Real properties start after it.

## Installing The Property

Properties are installed during class initialization:

```cpp
g_object_class_install_property(
    object_class,
    PROP_PRINT_PTS,
    g_param_spec_boolean(
        "print-pts",
        "Print PTS",
        "Print the presentation timestamp for each buffer",
        TRUE,
        G_PARAM_READWRITE
    )
);
```

The important pieces are:

- `"print-pts"` is the public property name.
- `TRUE` is the default value shown by `gst-inspect-1.0`.
- `G_PARAM_READWRITE` means callers can read and write the value.

## Set And Get Callbacks

GObject calls `set_property` when someone writes a property:

```cpp
object_class->set_property = gst_property_filter_set_property;
```

The setter copies the external value into the instance field:

```cpp
self->print_pts = g_value_get_boolean(value);
```

GObject calls `get_property` when someone reads a property:

```cpp
object_class->get_property = gst_property_filter_get_property;
```

The getter copies the instance field back into a `GValue`:

```cpp
g_value_set_boolean(value, self->print_pts);
```

## Using The Property

The processing callback reads the instance field:

```cpp
if (!self->print_pts) {
    return GST_FLOW_OK;
}
```

That is the full loop:

```text
gst-launch property syntax
-> GObject set_property()
-> instance field
-> transform_ip() behavior
```

## Inspecting The Property

After building, run:

```sh
GST_PLUGIN_PATH="$PWD/build" gst-inspect-1.0 propertyfilter
```

The output lists `print-pts` under `Element Properties`.
