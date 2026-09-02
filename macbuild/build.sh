#!/bin/sh
# Runs NATIVELY on a macOS host (no container -- unlike winbuild/linuxbuild,
# there is no Linux-hosted cross-compiler for macOS).
#
# Builds a universal2 (x86_64 + arm64) BlitzView.app against the
# aqtinstall-provided Qt "macos" kit, which already ships as a universal
# binary itself -- one Qt download covers both CPU architectures.
#
# Qt bundles FFmpeg 7.x .dylib files in its lib/ directory but no headers,
# exactly like the Linux/Windows builds; fetch-ffmpeg-headers.sh gets the
# matching upstream headers.
set -eu

PROJECT=${PROJECT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
BUILD_DIR=${BUILD_DIR:-$PROJECT/build/macos}
BUILD_TYPE=${BUILD_TYPE:-Release}
QT_MACOS_PREFIX=${QT_MACOS_PREFIX:?"set QT_MACOS_PREFIX to the aqtinstall Qt macOS prefix (e.g. \$HOME/Qt/6.8.3/macos)"}
FFMPEG_INCLUDE_DIR=${FFMPEG_INCLUDE_DIR:-$BUILD_DIR/ffmpeg-include}

mkdir -p "$BUILD_DIR"

"$PROJECT/macbuild/fetch-ffmpeg-headers.sh" "$FFMPEG_INCLUDE_DIR"

# ---------------------------------------------------------------------------
# Configure. CMAKE_OSX_ARCHITECTURES builds both slices in one pass; Xcode's
# command line tools carry both x86_64 and arm64 SDK support regardless of
# which architecture the runner itself is.
#
# BuildInfo.h uses the normal native GenerateBuildInfo CMake target (see
# CMakeLists.txt) -- unlike the Windows/Linux containers, git is available
# here directly, so there is no host-pregeneration step to do.
# ---------------------------------------------------------------------------
echo "--- configuring ($BUILD_TYPE, Ninja, universal2):"
cmake \
    -S "$PROJECT" \
    -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
    -DCMAKE_PREFIX_PATH="$QT_MACOS_PREFIX" \
    -DFFMPEG_INCLUDE_DIR="$FFMPEG_INCLUDE_DIR" \
    -DFFMPEG_LIB_DIR="$QT_MACOS_PREFIX/lib"

echo "--- building:"
cmake --build "$BUILD_DIR" --parallel

# ---------------------------------------------------------------------------
# Structural self-check: prove the executable is a universal2 binary and show
# what it links. The FFmpeg entries here must be Qt's bundled .dylib names --
# if a second FFmpeg ever sneaks in, this is where it shows up.
# ---------------------------------------------------------------------------
APP="$BUILD_DIR/BlitzView.app"
EXE="$APP/Contents/MacOS/BlitzView"
test -f "$EXE"

echo "--- architectures:"
lipo -archs "$EXE"
case " $(lipo -archs "$EXE") " in
    *' x86_64 '*) case " $(lipo -archs "$EXE") " in *' arm64 '*) : ;; *) echo "ERROR: missing arm64 slice" >&2; exit 1 ;; esac ;;
    *) echo "ERROR: missing x86_64 slice" >&2; exit 1 ;;
esac

echo "--- linked FFmpeg libraries:"
otool -L "$EXE" | grep -E 'libav|libsw' || true
echo "--- linked Qt frameworks:"
otool -L "$EXE" | grep -i Qt || true

echo "BUILD OK: $APP"
