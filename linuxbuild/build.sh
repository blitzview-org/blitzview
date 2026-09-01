#!/bin/sh
# Runs INSIDE the container, with the project bind-mounted at /project.
# Builds BlitzView natively against the aqt-installed Qt 6.8.3.
#
# Qt 6.8 Linux bundles FFmpeg 7.x .so files (libavcodec.so.61 etc.) in its
# lib/ directory, but provides no headers. We use upstream FFmpeg 7.1.1 headers
# from /opt/ffmpeg/include and Qt's bundled .so files for linking — exactly
# the same strategy as the Windows build.
set -eu

. /usr/src/deps.env

# gcc-toolset-12 provides a newer GCC (12.x) and libstdc++ than AlmaLinux 8's
# default GCC 8.5.0. The enable script puts the toolset's bin/ first on PATH.
. /opt/rh/gcc-toolset-12/enable

PROJECT=${PROJECT:-/project}
BUILD_DIR=${BUILD_DIR:-$PROJECT/build/linux}
BUILD_TYPE=${BUILD_TYPE:-Release}

QT_PREFIX="/opt/qt/$QT_VERSION/gcc_64"

# ---------------------------------------------------------------------------
# Configure.
#
# BuildInfo.h is generated on the HOST (by the Makefile) into
# build/linux/generated/, which is exactly the configure output dir the
# container sees at /project/build/linux/generated/. CMake uses the file
# verbatim (BV_USE_PREGENERATED_BUILDINFO).
#
# FFMPEG_INCLUDE_DIR: upstream FFmpeg 7.x headers (from deps.sh).
# FFMPEG_LIB_DIR: Qt's bundled FFmpeg .so files — the ONLY copy in the package.
# ---------------------------------------------------------------------------
mkdir -p "$BUILD_DIR"

echo "--- configuring ($BUILD_TYPE, Ninja):"
cmake \
    -S "$PROJECT" \
    -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DFFMPEG_INCLUDE_DIR="/opt/ffmpeg/include" \
    -DFFMPEG_LIB_DIR="$QT_PREFIX/lib" \
    -DBV_USE_PREGENERATED_BUILDINFO=ON

# ---------------------------------------------------------------------------
# Build.
# ---------------------------------------------------------------------------
echo "--- building:"
cmake --build "$BUILD_DIR" --parallel

# ---------------------------------------------------------------------------
# Structural self-check: show linked FFmpeg libraries.
# Must be Qt's FFmpeg (.so.61), not the system's (.so.59).
# ---------------------------------------------------------------------------
EXE="$BUILD_DIR/blitzview"
test -f "$EXE"
echo "--- linked FFmpeg libraries:"
ldd "$EXE" | grep -E 'libav|libsw' || true
echo "--- linked Qt libraries:"
ldd "$EXE" | grep Qt || true

echo "BUILD OK: $EXE"
