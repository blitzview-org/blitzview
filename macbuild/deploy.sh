#!/bin/sh
# Runs NATIVELY on macOS, after build.sh. Turns the bare BlitzView.app into a
# portable package: macdeployqt copies Qt frameworks and the
# platform/imageformat/multimedia plugins in; this script then ad-hoc signs
# the whole bundle itself with a fixed --identifier (ad-hoc, no Apple
# developer certificate is involved). The result is wrapped
# into a "BlitzView/" directory alongside a sibling licenses/ folder -- the
# same top-level shape as winbuild/linuxbuild's portable packages (launcher/app +
# visible licenses/), so unzipping shows the license texts without having to
# dig into the .app bundle's Contents/Resources.
#
# BlitzView also links Qt's bundled FFmpeg .dylibs directly (video
# thumbnails); macdeployqt does not always follow those, so they are copied
# explicitly below -- same caveat as winbuild/deploy.sh and
# linuxbuild/deploy.sh. This is the only FFmpeg in the package.
set -eu

PROJECT=${PROJECT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
BUILD_DIR=${BUILD_DIR:-$PROJECT/build/macos}
QT_MACOS_PREFIX=${QT_MACOS_PREFIX:?"set QT_MACOS_PREFIX to the aqtinstall Qt macOS prefix"}

# KF_VERSION for the license README. Read from the same pinned file the
# Linux/Windows builder image uses, so the version is maintained in one place.
. "$PROJECT/docker/deps.env"

APP="$BUILD_DIR/BlitzView.app"
test -d "$APP"

echo "--- running macdeployqt:"
"$QT_MACOS_PREFIX/bin/macdeployqt" "$APP" -verbose=1

FRAMEWORKS_DIR="$APP/Contents/Frameworks"
mkdir -p "$FRAMEWORKS_DIR"

# kimageformats plugins (XCF, PSD, TGA, QOI, ...), built into the Qt prefix by
# build.sh. macdeployqt normally takes the whole imageformats directory along,
# but it decides that for itself -- copy whatever it left behind, so the macOS
# package never reads fewer formats than the Linux one. The ad-hoc signing step
# further down covers these files too, because it signs the whole bundle.
echo "--- kimageformats plugins:"
KIMG_DEST="$APP/Contents/PlugIns/imageformats"
mkdir -p "$KIMG_DEST"
kimg_count=0
for plug in "$QT_MACOS_PREFIX/plugins/imageformats/"kimg_*; do
    [ -e "$plug" ] || continue
    [ -e "$KIMG_DEST/$(basename "$plug")" ] || cp "$plug" "$KIMG_DEST/"
    kimg_count=$((kimg_count + 1))
done
[ "$kimg_count" -gt 0 ] || {
    echo "ERROR: no kimg_* plugins in the Qt prefix -- run macbuild/build.sh first" >&2
    exit 1
}
echo "  $kimg_count plugins in $KIMG_DEST"

# ---------------------------------------------------------------------------
# Copy Qt's bundled FFmpeg libraries not already deployed by macdeployqt.
#
# Qt ships each one under several names (e.g. libavcodec.61.19.100.dylib, the
# real file, and libavcodec.61.dylib, a symlink to it -- the same versioning
# scheme as the Linux .so files). A plain `cp` dereferences the symlink and
# writes a second full copy, silently doubling that library's size in the
# package (found by comparing file sizes in a real build: two 28 MB files
# instead of one). Resolve one level of symlink and recreate it instead, the
# same dedup linuxbuild/deploy.sh's bundle_lib() does for its .so files.
# ---------------------------------------------------------------------------
echo "--- copying Qt's bundled FFmpeg libraries not already deployed:"
for lib in avcodec avformat avutil swscale swresample; do
    for dylib in "$QT_MACOS_PREFIX/lib/lib${lib}".*.dylib; do
        [ -e "$dylib" ] || continue

        real="$dylib"
        if [ -L "$dylib" ]; then
            target=$(readlink "$dylib")
            case "$target" in
                /*) real="$target" ;;
                *) real="$(dirname "$dylib")/$target" ;;
            esac
        fi
        real_name=$(basename "$real")
        alias_name=$(basename "$dylib")

        if [ ! -e "$FRAMEWORKS_DIR/$real_name" ]; then
            cp "$real" "$FRAMEWORKS_DIR/$real_name"
            echo "  + $real_name"
        fi
        if [ "$alias_name" != "$real_name" ] && [ ! -e "$FRAMEWORKS_DIR/$alias_name" ]; then
            ln -s "$real_name" "$FRAMEWORKS_DIR/$alias_name"
            echo "  ~ $alias_name -> $real_name"
        fi
    done
done

# ---------------------------------------------------------------------------
# Wrap the finished .app into a "BlitzView/" portable directory, licenses/
# alongside it as a visible sibling (not inside Contents/Resources).
# ---------------------------------------------------------------------------
PORTABLE_DIR="$BUILD_DIR/BlitzView"
rm -rf "$PORTABLE_DIR"
mkdir -p "$PORTABLE_DIR"
mv "$APP" "$PORTABLE_DIR/BlitzView.app"
APP="$PORTABLE_DIR/BlitzView.app"
FRAMEWORKS_DIR="$APP/Contents/Frameworks"

# ---------------------------------------------------------------------------
# Ad-hoc sign with a fixed --identifier. The "-" signing identity means no
# Apple Developer certificate is needed; the app is not notarized, so users
# get Gatekeeper's "unidentified developer" prompt on first launch.
# Without an explicit identifier, TCC (the "Allow
# BlitzView to access X?" folder-permission prompts) has no stable identity
# to remember grants against, so the app can end up re-prompting on every
# single launch instead of just once per copy. --deep re-signs the bundled
# Qt frameworks and FFmpeg dylibs too, not just the main executable.
# ---------------------------------------------------------------------------
echo "--- ad-hoc signing:"
codesign --force --deep --sign - --identifier org.blitzview.blitzview "$APP"
codesign --verify --verbose=2 "$APP"

# ---------------------------------------------------------------------------
# License texts -- ship the component licenses plus a platform-specific
# README with the source-offer / upstream links. Only BlitzView, Qt and
# FFmpeg: macOS uses Cocoa natively (no X11/xcb/xkbcommon family) and Qt's
# macOS package does not bundle a separate ICU.
#
# Shipping a binary WITHOUT these texts violates the GPL/LGPL, so a missing
# directory is a hard build failure -- never a silent skip.
# ---------------------------------------------------------------------------
echo "--- copying licenses:"
mkdir -p "$PORTABLE_DIR/licenses"

QT_VERSION=$("$QT_MACOS_PREFIX/bin/qmake" -query QT_VERSION 2>/dev/null || true)
if [ -z "$QT_VERSION" ]; then
    QT_VERSION=$(basename "$(dirname "$QT_MACOS_PREFIX")")
fi
QT_SERIES=$(echo "$QT_VERSION" | cut -d. -f1,2)

# Gate the license list against what is actually in Frameworks/. The Qt
# libraries themselves are .framework DIRECTORIES and the plugins live in
# Contents/PlugIns, so the only loose .dylib files here are meant to be the
# FFmpeg ones copied above. Anything else is a component macdeployqt pulled
# in that README-macos.txt.in does not name -- shipping it without its
# license text would be a license violation, so fail loudly rather than
# quietly widen the package. (This is the macOS counterpart to
# linuxbuild/deploy.sh's is_bundlable whitelist.)
unexpected=$(ls "$FRAMEWORKS_DIR"/*.dylib 2>/dev/null | xargs -n1 basename |
             grep -vE '^lib(av|sw)' || true)
if [ -n "$unexpected" ]; then
    echo "ERROR: unexpected bundled libraries with no license entry:" >&2
    echo "$unexpected" >&2
    echo "Add them to licenses/README-macos.txt.in (plus a licenses/ subdir) or stop bundling them." >&2
    exit 1
fi

# Only the fully versioned real files, not the major-version symlinks next to
# them (libavcodec.61.19.100.dylib, not libavcodec.61.dylib) -- otherwise
# every library is listed twice. Same filter as linuxbuild/deploy.sh's
# libav*.so.*.*.* glob, and the leading '.' becomes a space so the output
# reads "libavcodec 61.19.100" exactly like the Linux package's README.
FFMPEG_LIBS=$(ls "$FRAMEWORKS_DIR"/libav*.*.*.*.dylib "$FRAMEWORKS_DIR"/libsw*.*.*.*.dylib 2>/dev/null |
              xargs -n1 basename | sed 's/\.dylib$//; s/\./ /' | sort | tr '\n' ',' | sed 's/,$//; s/,/, /g')
[ -n "$FFMPEG_LIBS" ] || { echo "ERROR: no bundled FFmpeg libraries found" >&2; exit 1; }

sed -e "s/@QT_VERSION@/$QT_VERSION/g" \
    -e "s/@QT_SERIES@/$QT_SERIES/g" \
    -e "s/@FFMPEG_LIBS@/$FFMPEG_LIBS/g" \
    -e "s/@KF_VERSION@/$KF_VERSION/g" \
    "$PROJECT/licenses/README-macos.txt.in" > "$PORTABLE_DIR/licenses/README.txt"
if grep -q '@[A-Z_]*@' "$PORTABLE_DIR/licenses/README.txt"; then
    echo "ERROR: unsubstituted placeholder in licenses/README.txt" >&2
    grep -n '@[A-Z_]*@' "$PORTABLE_DIR/licenses/README.txt" >&2
    exit 1
fi

for sub in BlitzView Qt FFmpeg kimageformats; do
    if [ ! -d "$PROJECT/licenses/$sub" ]; then
        echo "ERROR: licenses/$sub is missing -- the package must not ship without it" >&2
        exit 1
    fi
    cp -r "$PROJECT/licenses/$sub" "$PORTABLE_DIR/licenses/"
done
find "$PORTABLE_DIR/licenses" -type f | sort

# ---------------------------------------------------------------------------
# Self-check: every dylib the executable and the bundled Frameworks import
# must resolve to something inside the bundle (@rpath/@executable_path/
# @loader_path) or a system library under /usr/lib or /System. Anything else
# is a dependency macdeployqt or the FFmpeg copy step above missed.
# ---------------------------------------------------------------------------
echo "--- self-check: verifying all dependencies are bundled or system"
EXE="$APP/Contents/MacOS/BlitzView"
missing=0
for f in "$EXE" "$FRAMEWORKS_DIR"/*.dylib; do
    [ -f "$f" ] || continue
    # Dependency lines are tab-indented; header lines are not. Filtering on
    # that (rather than skipping a fixed number of lines) is what makes this
    # work for a universal2/fat file too -- otool -L prints one header PER
    # ARCHITECTURE ("... (architecture arm64):"), and a naive `tail -n +2`
    # only strips the first one, leaving the second header's own path to be
    # misread as a "dependency" of itself.
    deps=$(otool -L "$f" | grep '^	' | awk '{print $1}')
    for dep in $deps; do
        case "$dep" in
            /usr/lib/*|/System/*) continue ;;
            @rpath/*|@executable_path/*|@loader_path/*) continue ;;
        esac
        # A dylib's (but not an executable's) first entry is its own install
        # name (LC_ID_DYLIB) -- a self-reference, not a real dependency. The
        # real FFmpeg files above were placed with a plain `cp`, so their
        # install name is still whatever Qt's build set it to (possibly an
        # absolute path); that is fine, since it is never how another file
        # loads them.
        if [ "$(basename "$dep")" = "$(basename "$f")" ]; then
            continue
        fi
        echo "MISSING or unexpected absolute dependency in $(basename "$f"): $dep"
        missing=$((missing + 1))
    done
done
[ "$missing" -eq 0 ] || { echo "deployment incomplete"; exit 1; }

echo "--- portable package: $PORTABLE_DIR"
du -sh "$PORTABLE_DIR"
ls "$PORTABLE_DIR"
echo "PORTABLE OK"
