#!/bin/sh
# Runs INSIDE the container, after build.sh. Turns the bare blitzview binary
# into a portable directory that can run on any Linux with glibc >= 2.28:
# Qt libraries, Qt's bundled FFmpeg libraries, plugins.
#
# Key invariant: exactly ONE set of FFmpeg libraries — Qt's own bundled copy.
# We do NOT copy the system's FFmpeg (Debian bookworm: libavcodec.so.59),
# only Qt's (libavcodec.so.61). BlitzView links against the same set.
set -eu

. /usr/src/deps.env

PROJECT=${PROJECT:-/project}
BUILD_DIR=${BUILD_DIR:-$PROJECT/build/linux}
PORTABLE_DIR=${PORTABLE_DIR:-$BUILD_DIR/BlitzView}
DEPLOY_DIR=$PORTABLE_DIR/app

QT_PREFIX="/opt/qt/$QT_VERSION/gcc_64"

rm -rf "$PORTABLE_DIR"
mkdir -p "$DEPLOY_DIR/lib"
mkdir -p "$DEPLOY_DIR/platforms"
mkdir -p "$DEPLOY_DIR/imageformats"
mkdir -p "$DEPLOY_DIR/multimedia"
mkdir -p "$DEPLOY_DIR/xcbglintegrations"

# ---------------------------------------------------------------------------
# Copy the binary.
# ---------------------------------------------------------------------------
cp "$BUILD_DIR/blitzview" "$DEPLOY_DIR/"

# ---------------------------------------------------------------------------
# Discover all runtime dependencies via ldd.
# Copy everything that lives outside standard system paths.
# Skip libc, libstdc++, libgcc_s — these are system libraries that must NOT
# be bundled (bundling glibc breaks NSS, PAM, etc.).
# ---------------------------------------------------------------------------
echo "--- copying shared libraries:"

# Decide whether a shared library may be bundled into the portable package.
#
# We bundle ONLY what the target system is not guaranteed to provide in a
# compatible version:
#   - Qt libraries + Qt's private support libs + Qt plugins
#   - Qt's bundled FFmpeg libraries
#   - ICU (hard version lock: Qt 6.8.3 needs libicu{X}.so.73, but distros ship
#     ICU 74+, so the exact .73 major MUST be bundled or the app cannot load)
#
# Everything else is a system library (fontconfig, freetype, zlib, X11, glib,
# dbus, openssl, ...): they have stable, distro-agnostic sonames (libz.so.1,
# libX11.so.6) and the app should use the user's own copies. Bundling old
# versions of those only causes compatibility noise (e.g. the fontconfig
# "unknown element reset-dirs" warning from a bundled fontconfig 1.12).
#
# $1 = the library basename as ldd reports it (e.g. "libQt6Core.so.6").
is_bundlable() {
    case "$1" in
        libQt6*.so.*|libQt6*.so)                    return 0 ;;  # Qt + FFmpeg stubs
        libavcodec.so.*|libavformat.so.*|libavutil.so.*|\
        libswresample.so.*|libswscale.so.*)         return 0 ;;  # Qt's bundled FFmpeg
        libicuuc.so.7[1-9]*|libicui18n.so.7[1-9]*|\
        libicudata.so.7[1-9]*)                       return 0 ;;  # ICU, lock to .73 major
        # X11/xcb/xkbcommon family required by Qt's xcb platform plugin.
        # Not guaranteed present on minimal targets (e.g. Debian 12 without
        # libxcb-cursor0), so Qt's own dependency set must be bundled.
        libX11.so*|libX11-xcb.so*|libxcb.so*|libxcb-*.so*|\
        libxkbcommon.so*|libxkbcommon-x11.so*|libXext.so*|\
        libXcursor.so*|libXfixes.so*|libXrender.so*|libXrandr.so*|\
        libXi.so*|libXtst.so*|libXinerama.so*|libXau.so*|libxshmfence.so*|\
        libxcb-util.so*)                             return 0 ;;
    esac
    return 1
}

# Bundle one shared library into lib/. Copies the real file exactly once and
# creates symlinks for the versioned/short alias names (avoids triplicating
# e.g. libavcodec.so / .so.61 / .so.61.19.100, which bloated the package).
#
# $1 = the resolved real file path (e.g. /opt/qt/.../lib/libavcodec.so.61.19.100)
#     or a symlink path pointing at it.
bundle_lib() {
    local src=$1
    [ -e "$src" ] || return 0

    # Resolve to the real, versioned file we should store on disk.
    local real
    real=$(readlink -f "$src")
    [ -f "$real" ] || return 0

    local real_name
    real_name=$(basename "$real")

    # Copy the real file once.
    if [ ! -e "$DEPLOY_DIR/lib/$real_name" ]; then
        cp "$real" "$DEPLOY_DIR/lib/$real_name"
        echo "  + $real_name"
    fi

    # Symlink every alias the linker used (incl. the plain .so name).
    local alias_name
    alias_name=$(basename "$src")
    if [ "$alias_name" != "$real_name" ]; then
        if [ ! -e "$DEPLOY_DIR/lib/$alias_name" ] && [ ! -L "$DEPLOY_DIR/lib/$alias_name" ]; then
            ln -s "$real_name" "$DEPLOY_DIR/lib/$alias_name"
            echo "  ~ $alias_name -> $real_name"
        fi
    fi

    # Also ensure the unversioned .so symlink exists for this library.
    local short
    short=$(echo "$real_name" | sed -E 's/\.so\.[0-9].*/\.so/')
    if [ "$short" != "$real_name" ] && [ ! -e "$DEPLOY_DIR/lib/$short" ] && [ ! -L "$DEPLOY_DIR/lib/$short" ]; then
        ln -s "$real_name" "$DEPLOY_DIR/lib/$short"
        echo "  ~ $short -> $real_name"
    fi
}

# Copy every bundlable dependency of $1 into lib/.
copy_deps_of() {
    ldd "$1" | while IFS= read -r line; do
        lib=$(echo "$line" | awk '{print $1}')
        path=$(echo "$line" | awk '{print $3}')

        # Skip lines without a resolved path (e.g. "not found"), system libs,
        # and non-bundlable libraries.
        [ -z "$lib" ] || [ -z "$path" ] && continue
        is_bundlable "$lib" || continue
        [ ! -e "$path" ] && continue

        bundle_lib "$path"
    done
}

copy_deps_of "$DEPLOY_DIR/blitzview"

# Also copy Qt's bundled FFmpeg .so files explicitly.
# ldd may not show them if they're loaded at runtime by the Qt multimedia plugin,
# but the app needs them at runtime. These are the ONLY FFmpeg in the package.
echo "--- copying Qt's bundled FFmpeg libraries:"
for lib in avcodec avformat avutil swscale swresample; do
    for so in "$QT_PREFIX/lib/lib${lib}".so*; do
        [ -e "$so" ] || continue
        bundle_lib "$so"
    done
done

echo "--- libraries in lib/:"
ls -la "$DEPLOY_DIR/lib/"

# ---------------------------------------------------------------------------
# Copy Qt plugins and their dependencies.
#
# IMPORTANT: copy_deps_of must run on the ORIGINAL plugins in $QT_PREFIX,
# not on the deployed copies. The deployed copies have broken RPATH so
# ldd cannot resolve their Qt library deps (libQt6XcbQpa, libQt6Gui, etc.).
# ---------------------------------------------------------------------------
echo "--- copying plugin dependencies (from Qt prefix):"

copy_plugin_deps() {
    local plugin_dir="$QT_PREFIX/plugins/$1"
    local deploy_subdir="$2"
    for so in "$plugin_dir"/*.so; do
        [ -f "$so" ] || continue
        copy_deps_of "$so"
    done
}

copy_plugin_deps "platforms"         "$DEPLOY_DIR/platforms"
copy_plugin_deps "imageformats"      "$DEPLOY_DIR/imageformats"
copy_plugin_deps "multimedia"        "$DEPLOY_DIR/multimedia"
copy_plugin_deps "xcbglintegrations" "$DEPLOY_DIR/xcbglintegrations"

echo "--- copying Qt plugins to deploy dir:"

# Platform plugin (required for any display).
for pat in "$QT_PREFIX/plugins/platforms/libqxcb.so" \
           "$QT_PREFIX/plugins/platforms/libqminimal.so" \
           "$QT_PREFIX/plugins/platforms/libqoffscreen.so"; do
    [ -f "$pat" ] && cp "$pat" "$DEPLOY_DIR/platforms/"
done

# Image format plugins.
for f in "$QT_PREFIX/plugins/imageformats/"*.so; do
    [ -f "$f" ] && cp "$f" "$DEPLOY_DIR/imageformats/"
done

# Multimedia plugins (FFmpeg decoder etc.).
for f in "$QT_PREFIX/plugins/multimedia/"*.so; do
    [ -f "$f" ] && cp "$f" "$DEPLOY_DIR/multimedia/"
done

# XCB + OpenGL integration plugins.
for f in "$QT_PREFIX/plugins/xcbglintegrations/"*.so; do
    [ -f "$f" ] && cp "$f" "$DEPLOY_DIR/xcbglintegrations/"
done

echo "--- plugins copied:"
find "$DEPLOY_DIR" -name '*.so' -not -path '*/lib/*' | sort || true

# ---------------------------------------------------------------------------
# License texts — ship the component licenses plus a platform-specific README
# with the source-offer / upstream links. Linux bundles the X11/xcb/xkbcommon
# family for the xcb platform plugin, so all component subdirectories are
# copied here.
#
# Shipping a binary WITHOUT these texts violates the GPL/LGPL, so a missing
# directory is a hard build failure -- never a silent skip. (It used to be
# one, and a rename of licenses/libX11 -> licenses/xorg went unnoticed.)
#
# The README is a template: the component versions are substituted from what
# is actually bundled, so the source offer can never point at the wrong
# upstream tarball after a Qt bump.
# ---------------------------------------------------------------------------
echo "--- copying licenses:"
mkdir -p "$PORTABLE_DIR/licenses"

# Versions, read back from the deployed files themselves.
QT_SERIES=$(echo "$QT_VERSION" | cut -d. -f1,2)
ICU_VERSION=$(ls "$DEPLOY_DIR"/lib/libicuuc.so.* 2>/dev/null |
              sed -n 's/.*libicuuc\.so\.\([0-9][0-9.]*\)$/\1/p' | sort -V | tail -1)
[ -n "$ICU_VERSION" ] || { echo "ERROR: no bundled ICU found" >&2; exit 1; }
FFMPEG_LIBS=$(ls "$DEPLOY_DIR"/lib/libav*.so.*.*.* "$DEPLOY_DIR"/lib/libsw*.so.*.*.* 2>/dev/null |
              xargs -n1 basename | sed 's/\.so\./ /' | sort | tr '\n' ',' | sed 's/,$//; s/,/, /g')
[ -n "$FFMPEG_LIBS" ] || { echo "ERROR: no bundled FFmpeg libraries found" >&2; exit 1; }

sed -e "s/@QT_VERSION@/$QT_VERSION/g" \
    -e "s/@QT_SERIES@/$QT_SERIES/g" \
    -e "s/@ICU_VERSION@/$ICU_VERSION/g" \
    -e "s/@FFMPEG_LIBS@/$FFMPEG_LIBS/g" \
    "$PROJECT/licenses/README-linux.txt.in" > "$PORTABLE_DIR/licenses/README.txt"
if grep -q '@[A-Z_]*@' "$PORTABLE_DIR/licenses/README.txt"; then
    echo "ERROR: unsubstituted placeholder in licenses/README.txt" >&2
    grep -n '@[A-Z_]*@' "$PORTABLE_DIR/licenses/README.txt" >&2
    exit 1
fi

for sub in BlitzView Qt ICU FFmpeg xorg libxkbcommon; do
    if [ ! -d "$PROJECT/licenses/$sub" ]; then
        echo "ERROR: licenses/$sub is missing -- the package must not ship without it" >&2
        exit 1
    fi
    cp -r "$PROJECT/licenses/$sub" "$PORTABLE_DIR/licenses/"
done
find "$PORTABLE_DIR/licenses" -type f | sort

# ---------------------------------------------------------------------------
# qt.conf — tells Qt where to find plugins and libraries relative to the
# executable. Without this, Qt looks in its hard-coded prefix paths.
# ---------------------------------------------------------------------------
cat > "$DEPLOY_DIR/qt.conf" <<'QTEOF'
[Paths]
Plugins = .
Libraries = lib
QTEOF

echo "--- qt.conf:"
cat "$DEPLOY_DIR/qt.conf"

# ---------------------------------------------------------------------------
# Launcher script — the user-visible "BlitzView" entry point.
# Analogous to the Windows launcher.c: starts app/blitzview, forwards args.
# ---------------------------------------------------------------------------
cat > "$PORTABLE_DIR/BlitzView" <<'LAUNCHER'
#!/bin/sh
# BlitzView portable launcher.
# Start the real application with bundled Qt and FFmpeg libraries.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$SCRIPT_DIR/app"

export LD_LIBRARY_PATH="$APP_DIR/lib"
export QT_PLUGIN_PATH="$APP_DIR"
export QT_QPA_PLATFORM_PLUGIN_PATH="$APP_DIR/platforms"

exec "$APP_DIR/blitzview" "$@"
LAUNCHER
chmod +x "$PORTABLE_DIR/BlitzView"

# ---------------------------------------------------------------------------
# Self-check: every shared library the executable imports must be either
# in lib/ or a system library.
# ---------------------------------------------------------------------------
echo "--- self-check: verifying all dependencies are bundled"
missing=0
while IFS= read -r line; do
    lib=$(echo "$line" | awk '{print $1}')
    path=$(echo "$line" | awk '{print $3}')

    [ -z "$lib" ] || [ -z "$path" ] && continue
    is_bundlable "$lib" || continue

    if echo "$line" | grep -q "not found"; then
        echo "MISSING (not found): $lib"
        missing=$((missing + 1))
        continue
    fi

    [ ! -f "$path" ] && continue
    if [ ! -f "$DEPLOY_DIR/lib/$lib" ] && [ ! -L "$DEPLOY_DIR/lib/$lib" ]; then
        echo "MISSING: $lib (at $path)"
        missing=$((missing + 1))
    fi
done <<EOF
$(ldd "$DEPLOY_DIR/blitzview")
EOF

[ "$missing" -eq 0 ] || { echo "DEPLOYMENT INCOMPLETE: $missing missing"; exit 1; }

# Verify exactly one FFmpeg in lib/.
ffmpeg_count=$(ls "$DEPLOY_DIR/lib/" | grep -cE '^(libav|libsw)' || true)
echo "--- FFmpeg files in lib/: $ffmpeg_count"
[ "$ffmpeg_count" -gt 0 ] || echo "WARNING: no FFmpeg libraries in lib/"

echo "--- portable package: $PORTABLE_DIR"
du -sh "$PORTABLE_DIR"
echo ""
echo "Contents:"
find "$PORTABLE_DIR" -type f | sort | head -80
echo ""
echo "PORTABLE OK"
