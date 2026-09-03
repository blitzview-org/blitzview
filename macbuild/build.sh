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
# kimageformats — the image format plugins Qt does not ship (XCF, PSD, TGA,
# QOI, DDS, HDR, PCX, ...). Same pinned version as the Linux/Windows builder
# image, read from the same file so it is maintained in one place.
#
# Installed INTO the Qt prefix, so macdeployqt picks the plugins up with Qt's
# own ones -- the same arrangement as the other two platforms.
#
# The CMAKE_DISABLE_FIND_PACKAGE_* denials matter MORE here than in the
# container: a GitHub runner may well have libavif/OpenEXR/LibRaw from
# Homebrew lying around, those are single-architecture, and linking one would
# break the universal2 build. Denying them keeps the output independent of
# whatever the runner happens to have installed. Keep in sync with
# docker/deps.sh.
# ---------------------------------------------------------------------------
. "$PROJECT/docker/deps.env"

KF_SRC_DIR="$BUILD_DIR/kf-src"
KF_HOST_PREFIX="$BUILD_DIR/kf-host"

# Skipped when the plugins are already in the prefix. In CI the Qt prefix is a
# restored cache keyed on docker/deps.env -- the same file KF_VERSION lives in,
# so a version bump invalidates that cache and rebuilds these along with Qt,
# while an unchanged pin costs nothing on every later run.
if [ ! -f "$QT_MACOS_PREFIX/plugins/imageformats/kimg_xcf.dylib" ]; then
    echo "--- building kimageformats $KF_VERSION (universal2):"
    mkdir -p "$KF_SRC_DIR"
    (
        cd "$KF_SRC_DIR"
        curl -L -o ecm-src.tar.xz "$ECM_SRC_URL"
        shasum -a 256 -c - <<EOF
$ECM_SRC_SHA256  ecm-src.tar.xz
EOF
        curl -L -o kimageformats-src.tar.xz "$KIMAGEFORMATS_SRC_URL"
        shasum -a 256 -c - <<EOF
$KIMAGEFORMATS_SRC_SHA256  kimageformats-src.tar.xz
EOF
        tar xJf ecm-src.tar.xz
        tar xJf kimageformats-src.tar.xz
    )

    # ECM is pure CMake modules; its doc targets need Sphinx and fail without it.
    cmake -S "$KF_SRC_DIR/extra-cmake-modules-$KF_VERSION" -B "$BUILD_DIR/b-ecm" \
        -G Ninja -Wno-dev \
        -DCMAKE_INSTALL_PREFIX="$KF_HOST_PREFIX" \
        -DBUILD_TESTING=OFF \
        -DBUILD_HTML_DOCS=OFF -DBUILD_MAN_DOCS=OFF -DBUILD_QTHELP_DOCS=OFF
    cmake --build "$BUILD_DIR/b-ecm" --target install

    cmake -S "$KF_SRC_DIR/kimageformats-$KF_VERSION" -B "$BUILD_DIR/b-kif" \
        -G Ninja -Wno-dev \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
        -DCMAKE_INSTALL_PREFIX="$QT_MACOS_PREFIX" \
        -DCMAKE_PREFIX_PATH="$QT_MACOS_PREFIX;$KF_HOST_PREFIX" \
        -DBUILD_TESTING=OFF \
        -DKIMAGEFORMATS_DDS=ON \
        -DKIMAGEFORMATS_JXL=OFF \
        -DKIMAGEFORMATS_JXR=OFF \
        -DKIMAGEFORMATS_HEIF=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_libavif=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_OpenEXR=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_LibRaw=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_KF6Archive=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_OpenJPEG=ON
    cmake --build "$BUILD_DIR/b-kif" --parallel
    cmake --install "$BUILD_DIR/b-kif"

    # ECM's plugin install dir differs per platform; normalise to where Qt
    # looks. Same fix as in docker/deps.sh.
    if [ -d "$QT_MACOS_PREFIX/lib/plugins/imageformats" ]; then
        mkdir -p "$QT_MACOS_PREFIX/plugins/imageformats"
        mv "$QT_MACOS_PREFIX/lib/plugins/imageformats/"kimg_* \
           "$QT_MACOS_PREFIX/plugins/imageformats/"
        rm -rf "$QT_MACOS_PREFIX/lib/plugins"
    fi
fi

echo "--- kimageformats plugins in the Qt prefix:"
ls "$QT_MACOS_PREFIX/plugins/imageformats/" | grep '^kimg_' || {
    echo "ERROR: kimageformats build produced no plugins" >&2
    exit 1
}

# Every plugin must be universal2 and free of non-Qt dependencies -- a
# single-architecture plugin would make the package fail on the other CPU.
for plug in "$QT_MACOS_PREFIX/plugins/imageformats/"kimg_*; do
    archs=$(lipo -archs "$plug")
    case " $archs " in
        *' x86_64 '*) case " $archs " in *' arm64 '*) : ;; *) false ;; esac ;;
        *) false ;;
    esac || { echo "ERROR: $plug is not universal2 ($archs)" >&2; exit 1; }
    if otool -L "$plug" | grep -qE 'libavif|libheif|libjxl|libraw|OpenEXR|Imath|libopenjp2|KF6Archive'; then
        echo "ERROR: $plug pulled in an optional codec dependency:" >&2
        otool -L "$plug" >&2
        exit 1
    fi
done

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
