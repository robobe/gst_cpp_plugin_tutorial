# Cross-compile NanoTrack for Radxa RK3566

Run the build on the x86-64 development PC using its AArch64 compiler. Headers
and libraries come from the Radxa sysroot; the resulting executable runs on the
board. The board's installed packages were not changed during this setup.

## Sysroot location and changes

The requested `/home/user/sysroot` path did not exist. The existing board
snapshot was `/home/user/sysroots/radxa` (approximately 45 GB), so the setup
created an alias instead of duplicating it:

```bash
ln -s /home/user/sysroots/radxa /home/user/sysroot
```

This command records the one-time change; do not rerun it once the alias exists.
Updates through either path affect the same sysroot.

Inspection found the required headers, CMake/pkg-config metadata, and shared
library binaries already present. No missing header or new library binary
needed downloading. A checksum-based sync from `radxa` restored the following
13 library symlinks, which the snapshot had stored as regular files. Paths in
this table are relative to `/usr/lib/aarch64-linux-gnu/` on both the board and
sysroot:

| Copied symlink | Target |
| --- | --- |
| `libopencv_core.so` | `libopencv_core.so.406` |
| `libopencv_core.so.406` | `libopencv_core.so.4.6.0` |
| `libopencv_imgproc.so` | `libopencv_imgproc.so.406` |
| `libopencv_imgproc.so.406` | `libopencv_imgproc.so.4.6.0` |
| `libopencv_highgui.so` | `libopencv_highgui.so.406` |
| `libopencv_highgui.so.406` | `libopencv_highgui.so.4.6.0` |
| `libopencv_videoio.so` | `libopencv_videoio.so.406` |
| `libopencv_videoio.so.406` | `libopencv_videoio.so.4.6.0` |
| `libopencv_imgcodecs.so` | `libopencv_imgcodecs.so.406` |
| `libopencv_imgcodecs.so.406` | `libopencv_imgcodecs.so.4.6.0` |
| `librga.so.2` | `librga.so.2.1.0` |
| `librknnrt.so` | `librknnrt.so.2.3.0` |
| `librknnrt.so.1` | `librknnrt.so.2.3.0` |

The sync also updated parent-directory timestamps. It did not delete unrelated
sysroot files. Restoring the links preserves the board's library naming and
version relationships; the initial build also worked with the previous copies.

## Headers and libraries required

These paths were compared with the board and synchronized as needed. Prefix
them with `/home/user/sysroot` to locate their host-side copies.

| Board path | Purpose / observed version |
| --- | --- |
| `/usr/include/opencv4/` | OpenCV headers, including core, imgproc, highgui, and videoio; OpenCV 4.6.0. |
| `/usr/include/rga/` | RGA import, buffer, resize, status, and format declarations, including `im2d.h` and `rga.h`. |
| `/usr/include/rknn_api.h` | RKNN contexts, tensor attributes, allocation/import, binding, inference, and `rknn_mem_sync`. |
| `/usr/lib/aarch64-linux-gnu/cmake/opencv4/` | OpenCV CMake package configuration and imported targets. |
| `/usr/lib/aarch64-linux-gnu/pkgconfig/opencv4.pc` | OpenCV pkg-config metadata, also used by the direct compiler commands in the README. |
| `/usr/lib/aarch64-linux-gnu/pkgconfig/librga.pc` | RGA include/link flags; reports version 2.1.0. |
| `/usr/lib/aarch64-linux-gnu/libopencv_{core,imgproc,highgui,videoio,imgcodecs}.so*` | Direct OpenCV dependencies plus imgcodecs used transitively; binary version 4.6.0, SONAME suffix 406. |
| `/usr/lib/aarch64-linux-gnu/librga.so*` | RGA runtime and linker name; versioned file `librga.so.2.1.0`. |
| `/usr/lib/aarch64-linux-gnu/librknnrt.so*` | RKNN runtime and linker names; versioned file `librknnrt.so.2.3.0`. |

On this board, Debian lists `librga-dev` and `librga2` as package version
`2.2.0-1`, while its pkg-config metadata and shared-library filename report
`2.1.0`. These are observed values, not a request to replace the board packages.
OpenCV packages report `4.6.0+dfsg-12`.

The existing snapshot also supplies the target C library, loader, and transitive
OpenCV dependencies (video codecs, GUI libraries, and related libraries).
The list above is an incremental refresh for this existing sysroot, not a
complete recipe for constructing a new sysroot from an empty directory.

## Repeat the targeted synchronization

Run on the development PC. SSH alias `radxa` uses the existing configured user
and key. This copies from the board to the host only, preserves symlinks, and
uses checksums to avoid transferring identical contents:

```bash
ssh -x -o BatchMode=yes radxa '
  find /usr/include/opencv4 /usr/include/rga \
    /usr/lib/aarch64-linux-gnu/cmake/opencv4 -type f
  printf "%s\n" \
    /usr/include/rknn_api.h \
    /usr/lib/aarch64-linux-gnu/pkgconfig/opencv4.pc \
    /usr/lib/aarch64-linux-gnu/pkgconfig/librga.pc \
    /usr/lib/aarch64-linux-gnu/librga.so* \
    /usr/lib/aarch64-linux-gnu/librknnrt.so* \
    /usr/lib/aarch64-linux-gnu/libopencv_core.so* \
    /usr/lib/aarch64-linux-gnu/libopencv_imgproc.so* \
    /usr/lib/aarch64-linux-gnu/libopencv_highgui.so* \
    /usr/lib/aarch64-linux-gnu/libopencv_videoio.so* \
    /usr/lib/aarch64-linux-gnu/libopencv_imgcodecs.so*
' > /tmp/nanotrack-sysroot-files.txt

rsync -aicR --no-owner --no-group \
  --files-from=/tmp/nanotrack-sysroot-files.txt \
  radxa:/ /home/user/sysroot/
```

To verify without writing, repeat the rsync command with `-anicR`. No output
means the selected contents, links, and compared metadata match. Do not use
`--copy-links`: the relative library links should remain links.

## Configure and build

The host needs CMake, Ninja, pkg-config, and `aarch64-linux-gnu-gcc-12` / `aarch64-linux-gnu-g++-12`. The verified
compiler is Ubuntu GCC 12.4.0. Run from the repository root:

```bash
cmake -S radxa/nano -B build-radxa-nano -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/radxa-zero3w.cmake" \
  -DRADXA_SYSROOT=/home/user/sysroot \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-radxa-nano
```

The existing [toolchain](../../cmake/toolchains/radxa-zero3w.cmake) selects Linux
AArch64, the host's GCC 12 cross compiler, and `--sysroot`. It restricts CMake library,
header, and package searches to the target sysroot while allowing host build
tools. It sets `PKG_CONFIG_SYSROOT_DIR` and target-only `PKG_CONFIG_LIBDIR`.
It also clears `PKG_CONFIG_PATH` to exclude host package overrides.
The explicit `RADXA_SYSROOT` overrides the toolchain's older default path.

An initial build with the default GCC 13 linked but failed to load on the board:
`GLIBCXX_3.4.32` and `GLIBC_2.38` were unavailable. `--sysroot` alone did not
exclude Ubuntu's cross-toolchain C headers and runtime libraries. The corrected
toolchain selects GCC 12, explicitly prioritizes the sysroot's `/usr/include`
and `/usr/include/aarch64-linux-gnu`, and adds library search directories for
`/usr/lib/aarch64-linux-gnu`, `/lib/aarch64-linux-gnu`, and
`/usr/lib/gcc/aarch64-linux-gnu/12` under the sysroot. The last directory supplies
the board's `libstdc++.so` linker file; GCC 12 alone still selected a host-supplied
cross runtime built against newer glibc without that search-path correction.
The existing target `stdio.h`, libc header, libc, and libstdc++ were checksum
compared with the board and already matched; no replacement was needed.

These changes apply to other consumers of this shared toolchain as well: they
now require the installed GCC 12 cross compiler and the board's GCC 12 runtime
files in their sysroot.

The corrected [CMakeLists.txt](CMakeLists.txt) builds one executable from
`nanotrack_rknn_rga.cpp`, finds only the required OpenCV components, imports RGA
through pkg-config, and locates RKNN headers/library within the sysroot. The
unrelated GStreamer plugin targets and their nonexistent source paths were
removed. `SKIP_BUILD_RPATH` prevents embedding host sysroot library paths in the
board executable. The same CMake target can also build natively on the board by
omitting the toolchain and sysroot arguments.

Use a separate build directory when changing compiler or sysroot; CMake caches
compiler and dependency paths. For an existing directory on CMake 3.24 or newer,
`cmake --fresh` with the same configure arguments resets that cache. Inspect `build-radxa-nano/compile_commands.json`
if a header unexpectedly comes from the host.

## Verify and run on the board

```bash
file build-radxa-nano/nanotrack_rknn_rga
aarch64-linux-gnu-readelf -d build-radxa-nano/nanotrack_rknn_rga
```

Expect an ARM AArch64 ELF executable with interpreter
`/lib/ld-linux-aarch64.so.1`. The dynamic section should name target OpenCV,
RKNN, RGA, and system libraries, with no host RPATH/RUNPATH.

For a temporary loader smoke test:

```bash
RADXA_CHECK_DIR=$(ssh -x -o BatchMode=yes radxa \
  'mktemp -d /tmp/nanotrack-cross-check.XXXXXX')
scp build-radxa-nano/nanotrack_rknn_rga "radxa:$RADXA_CHECK_DIR/"
ssh -x -o BatchMode=yes radxa "$RADXA_CHECK_DIR/nanotrack_rknn_rga --help"
ssh -x -o BatchMode=yes radxa \
  "rm '$RADXA_CHECK_DIR/nanotrack_rknn_rga' && rmdir '$RADXA_CHECK_DIR'"
```

For tracking, deploy the executable to your chosen board directory and use the
[README usage instructions](README.md#run), specifying board-local video and
model paths. `--resize cpu` still uses RKNN and links librga. Headless tracking
requires both `--no-display` and `--roi`.

Validation performed for this change: CMake configure and Release cross-build
passed; `file`/`readelf` confirmed AArch64 and no embedded RPATH/RUNPATH; a second
checksum rsync dry run found no differences. The copied executable's `--help`
was tested on the board to check loading and CLI startup. This is not a model
inference or tracking-accuracy test.
