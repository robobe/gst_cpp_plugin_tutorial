#!/usr/bin/env bash
# Run on RK3566 with GST_PLUGIN_PATH pointing to this plugin build.
set -euo pipefail
model=${1:?Usage: bash check-fallback.sh /path/to/yolov8n.rknn}
log=$(mktemp)
trap 'rm -f "$log"' EXIT
if ! GST_DEBUG_NO_COLOR=1 GST_DEBUG=rknnyolodetect:3 gst-launch-1.0 -q \
    videotestsrc num-buffers=3 ! video/x-raw,format=RGB,width=16,height=16 ! \
    rknnyolodetect model-path="$model" ! fakesink >"$log" 2>&1; then
    cat "$log"
    exit 1
fi
if [ "$(grep -c 'using CPU resize until stop' "$log")" != 1 ]; then
    cat "$log"
    echo 'Expected exactly one RGA-to-CPU fallback warning.' >&2
    exit 1
fi
echo 'PASS: RGA failure falls back to CPU and the three-frame pipeline completes.'
