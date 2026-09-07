# Using RKNN YOLO Detection on Radxa Zero 3W

See the [plugin README](../../src/rknnyolodetect/README.md) for source explanations,
model requirements, properties, and metadata usage.

## Prepare the Board and Sysroot

Install only the development packages that do not disturb the board's existing
DRM/Mesa stack:

```bash
ssh radxa
sudo apt install -y libgstreamer1.0-dev librga-dev
exit
```

The board's `libgstreamer-plugins-base1.0-dev` dependencies conflict with its
newer graphics libraries. Do not install or downgrade those graphics packages.
Instead, copy the installed development files and extract the required package
directly into the cross sysroot:

```bash
RADXA_SYSROOT="$HOME/sysroots/radxa"

ssh radxa '
  for package in \
    libgstreamer1.0-dev librga-dev libglib2.0-dev libpcre2-dev libffi-dev \
    libmount-dev libselinux1-dev libunwind-dev uuid-dev libblkid-dev \
    libdw-dev libelf-dev; do
    dpkg-query -L "$package"
  done | while read -r path; do
    if test ! -d "$path"; then printf "%s\n" "${path#/}"; fi
  done
' | sort -u > /tmp/radxa-dev-files.txt

rsync -aR --files-from=/tmp/radxa-dev-files.txt \
  radxa:/ "$RADXA_SYSROOT/"

curl -fsSL \
  https://radxa-repo.github.io/rk3566-bookworm/pool/main/g/gst-plugins-base1.0/libgstreamer-plugins-base1.0-dev_1.22.9-1_arm64.deb \
  -o /tmp/gstreamer-base-dev.deb
echo "ea0ca97d38a52f38c790ae1b98a9fc78e42eeec4dccb67ce07976fd707e52631  /tmp/gstreamer-base-dev.deb" \
  | sha256sum -c -
dpkg-deb -x /tmp/gstreamer-base-dev.deb "$RADXA_SYSROOT"

curl -fsSL \
  https://deb.debian.org/debian/pool/main/o/orc/liborc-0.4-dev_0.4.33-2_arm64.deb \
  -o /tmp/orc-dev.deb
echo "9452fef6140e0c3a08aa0f9cc687e564e5f0d613717f70d1d94659f88ad29571  /tmp/orc-dev.deb" \
  | sha256sum -c -
dpkg-deb -x /tmp/orc-dev.deb "$RADXA_SYSROOT"
```

## Cross-Compile

Run from the repository root:

```bash
cmake -S src/rknnyolodetect -B build-radxa-yolo -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/radxa-zero3w.cmake" \
  -DRADXA_SYSROOT="$HOME/sysroots/radxa"
cmake --build build-radxa-yolo
```

## Deploy

```bash
ssh radxa 'mkdir -p ~/gst-yolo-demo/{plugins,assets,models}'
scp build-radxa-yolo/libgstrknnyolodetect.so build-radxa-yolo/libgstmetaprint.so \
  radxa:gst-yolo-demo/plugins/
scp assets/bus.jpg radxa:gst-yolo-demo/assets/
scp /home/user/Downloads/yolov8n.rknn radxa:gst-yolo-demo/models/
```

## Inspect and Run

```bash
ssh radxa
export GST_PLUGIN_PATH="$HOME/gst-yolo-demo/plugins"
gst-inspect-1.0 rknnyolodetect

gst-launch-1.0 -q \
  filesrc location="$HOME/gst-yolo-demo/assets/bus.jpg" ! \
  jpegdec ! videoconvert ! video/x-raw,format=RGB ! \
  rknnyolodetect model-path="$HOME/gst-yolo-demo/models/yolov8n.rknn" ! \
  metaprint ! fakesink
```

Expected detections include COCO class `0` for people and class `5` for the
bus. Counts and confidence values may differ slightly from ONNX Runtime because
the RKNN model is quantized.

Enable timing logs:

```bash
GST_DEBUG_NO_COLOR=1 GST_DEBUG=rknnyolodetect:6 \
gst-launch-1.0 -q \
  filesrc location="$HOME/gst-yolo-demo/assets/bus.jpg" ! \
  jpegdec ! videoconvert ! video/x-raw,format=RGB ! \
  rknnyolodetect model-path="$HOME/gst-yolo-demo/models/yolov8n.rknn" ! \
  metaprint ! fakesink
```

The plugin publishes metadata only; drawing remains the responsibility of a
separate overlay element.
