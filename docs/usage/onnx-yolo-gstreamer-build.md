# Build GStreamer ONNX YOLO Elements

This builds a current GStreamer environment alongside Ubuntu's system
installation. It provides these elements:

```text
onnxinference ! yolov8tensordec ! objectdetectionoverlay
```

Run the commands from the repository root unless a command changes directory.

## 1. Upgrade Meson Used by This Project

The current GStreamer source requires Meson 1.4 or newer. Upgrade Meson in the
existing project virtual environment, then place that environment before the
system tools in `PATH`:

```bash
python3 -m pip install --upgrade 'meson>=1.4'
export PATH="$PWD/.venv/bin:$PATH"
hash -r
meson --version
```

Continue only when `meson --version` prints `1.4` or newer. If Meson reported
`1.3.2 but project requires >= 1.4`, rerun these four commands in the same
shell before configuring GStreamer.

## 2. Install Build Dependencies

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake git ninja-build pkg-config \
  python3 flex bison gettext \
  libglib2.0-dev liborc-0.4-dev libpango1.0-dev libeigen3-dev
```

The prepended `.venv/bin/meson` from the previous step is used for this build.

## 3. Build ONNX Runtime

Install the CPU ONNX Runtime shared library under a user-local prefix:

```bash
ORT_PREFIX="$HOME/opt/onnxruntime"
mkdir -p "$HOME/src" "$HOME/build" "$HOME/opt"

git clone --recursive --branch v1.29.0 \
  https://github.com/microsoft/onnxruntime.git "$HOME/src/onnxruntime"

cmake -S "$HOME/src/onnxruntime/cmake" -B "$HOME/build/onnxruntime" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$ORT_PREFIX" \
  -Donnxruntime_BUILD_SHARED_LIB=ON \
  -DBUILD_TESTING=OFF \
  -Donnxruntime_BUILD_UNIT_TESTS=OFF \
  -Donnxruntime_USE_PREINSTALLED_EIGEN=ON \
  -Deigen_SOURCE_PATH=/usr/include/eigen3

cmake --build "$HOME/build/onnxruntime"
cmake --install "$HOME/build/onnxruntime"
```

## 4. Build GStreamer With ONNX and YOLO Decoding

```bash
git clone --depth 1 https://gitlab.freedesktop.org/gstreamer/gstreamer.git \
  "$HOME/src/gstreamer"

cd "$HOME/src/gstreamer"
PKG_CONFIG_PATH="$ORT_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
meson setup build \
  -Dgst-plugins-bad:onnx=enabled \
  -Dgst-plugins-bad:tensordecoders=enabled \
  -Dgst-plugins-bad:analyticsoverlay=enabled

meson compile -C build
```

## 5. Verify the New Elements

`meson devenv` starts a shell that finds only the new build's tools, libraries,
and plugins. It does not replace the Ubuntu GStreamer installation.

```bash
cd "$HOME/src/gstreamer"
meson devenv -C build

gst-inspect-1.0 onnxinference
gst-inspect-1.0 yolov8tensordec
gst-inspect-1.0 objectdetectionoverlay
```

Each command must print factory details. Exit this shell to return to the
system GStreamer environment.

## 6. Next Step

The YOLO model must expose the tensor IDs and output layout expected by
`yolov8tensordec`. With a compatible model and label file, the pipeline shape
is:

```bash
... ! onnxinference model-file=yolov8n.onnx ! \
  yolov8tensordec label-file=coco.txt ! \
  objectdetectionoverlay ! autovideosink
```
