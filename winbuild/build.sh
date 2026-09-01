#!/bin/sh
# Runs INSIDE the container, with the project bind-mounted at /project.
#
# Cross-compiles BlitzView for Windows using a Linux-hosted MinGW compiler.
# The compiler runs natively on Linux — no Wine overhead per compile step.
# Wine is only used for Qt host tools (moc, uic, rcc) via CMake's
# CMAKE_CROSSCOMPILING_EMULATOR mechanism.
set -eu

. /usr/src/deps.env

PROJECT=${PROJECT:-/project}
BUILD_DIR=${BUILD_DIR:-$PROJECT/build/windows}
BUILD_TYPE=${BUILD_TYPE:-Release}

QT_PREFIX_WIN="/opt/qt/$QT_VERSION/mingw_64"
QT_PREFIX_HOST="/opt/qt/$QT_VERSION/gcc_64"

# ---------------------------------------------------------------------------
# Configure. FFMPEG_ROOT points at the headers from deps.sh plus the import
# libraries generated from Qt's own FFmpeg DLLs by importlibs.sh.
#
# BuildInfo.h is generated on the HOST (by the Makefile) into
# build/windows/generated/, which is exactly the configure output dir the
# container sees at /project/build/windows/generated/. CMake picks that file
# up verbatim (BV_USE_PREGENERATED_BUILDINFO); the builder image deliberately
# has no git, so no commit/date plumbing flows through here anymore.
# ---------------------------------------------------------------------------
mkdir -p "$BUILD_DIR"

echo "--- configuring ($BUILD_TYPE, Ninja, MinGW cross):"
cmake \
    -S "$PROJECT" \
    -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PROJECT/cmake/toolchain-mingw.cmake" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX_WIN" \
    -DQT_HOST_PATH="$QT_PREFIX_HOST" \
    -DFFMPEG_INCLUDE_DIR="/opt/ffmpeg/include" \
    -DFFMPEG_LIB_DIR="/opt/ffmpeg/lib" \
    -DBV_USE_PREGENERATED_BUILDINFO=ON

# ---------------------------------------------------------------------------
# Build.
# ---------------------------------------------------------------------------
echo "--- building:"
cmake --build "$BUILD_DIR" --parallel

# ---------------------------------------------------------------------------
# Structural self-check: prove we produced a PE executable and show what it
# imports. The FFmpeg DLL names here must be Qt's versioned ones — if a second
# FFmpeg ever sneaks in, this is where it shows up.
# ---------------------------------------------------------------------------
EXE="$BUILD_DIR/blitzview.exe"
test -f "$EXE"
echo "--- imported DLLs:"
x86_64-w64-mingw32-objdump -p "$EXE" | grep 'DLL Name' | sort

echo "BUILD OK: $EXE"
