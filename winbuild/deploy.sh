#!/bin/sh
# Runs INSIDE the container, after build.sh. Turns the bare blitzview.exe into
# a directory that can actually run: Qt DLLs, platform/imageformat/multimedia
# plugins, and the FFmpeg DLLs.
#
# windeployqt is a Windows executable that runs under Wine — it is the real
# Windows tool here (that is why QT_HOST_PATH points to the Linux Qt for
# cross-compilation, but deployment needs the actual Windows DLL inspection).
set -eu

. /usr/src/deps.env

PROJECT=${PROJECT:-/project}
BUILD_DIR=${BUILD_DIR:-$PROJECT/build/windows}
# The portable layout: BlitzView.exe (launcher) at the top, everything else
# tucked into app/.
PORTABLE_DIR=${PORTABLE_DIR:-$BUILD_DIR/BlitzView}
DEPLOY_DIR=$PORTABLE_DIR/app

QT_PREFIX_WIN="/opt/qt/$QT_VERSION/mingw_64"
MINGW_RUNTIME="/opt/mingw/x86_64-w64-mingw32/lib"

# Wine must find the Qt and MinGW runtime DLLs when running windeployqt.exe.
# Qt's bin/ is here too, so windeployqt can load the Qt libraries it inspects.
WINEPATH="$QT_PREFIX_WIN/bin;$MINGW_RUNTIME"
export WINEPATH

rm -rf "$PORTABLE_DIR"
mkdir -p "$DEPLOY_DIR"
cp "$BUILD_DIR/blitzview.exe" "$DEPLOY_DIR/"
# Renamed on copy — see the case-insensitivity note in CMakeLists.txt.
cp "$BUILD_DIR/blitzview-launcher.exe" "$PORTABLE_DIR/BlitzView.exe"

# Run windeployqt under Wine — it inspects the PE executable and copies
# the Qt DLLs, plugins, and platform files it needs.
wine "$QT_PREFIX_WIN/bin/windeployqt.exe" \
    --release \
    --no-translations \
    --no-system-d3d-compiler \
    --no-opengl-sw \
    "$DEPLOY_DIR/blitzview.exe"

# MinGW runtime DLLs. These come from the cross-compiler, not Qt, so
# windeployqt does not know about them: the AmanoTeam toolchain names its
# libstdc++ "libstdc++.dll" (not "libstdc++-6.dll") and uses libgcc_s_seh.dll
# for SEH unwinding. blitzview.exe imports both directly.
for dll in libgcc_s_seh.dll libstdc++.dll libwinpthread.dll; do
    [ -f "$MINGW_RUNTIME/$dll" ] || continue
    [ -f "$DEPLOY_DIR/$dll" ] || cp "$MINGW_RUNTIME/$dll" "$DEPLOY_DIR/"
done

# windeployqt resolves Qt's own dependencies. BlitzView additionally imports
# the FFmpeg DLLs directly (video thumbnails), and depending on the Qt version
# windeployqt does not always follow those. Copy whatever is missing — this is
# the same single set of DLLs Qt Multimedia uses, never a second copy.
for dll in "$QT_PREFIX_WIN/bin/avcodec"-*.dll \
           "$QT_PREFIX_WIN/bin/avformat"-*.dll \
           "$QT_PREFIX_WIN/bin/avutil"-*.dll \
           "$QT_PREFIX_WIN/bin/swscale"-*.dll \
           "$QT_PREFIX_WIN/bin/swresample"-*.dll; do
    [ -f "$dll" ] || continue
    [ -f "$DEPLOY_DIR/$(basename "$dll")" ] || cp "$dll" "$DEPLOY_DIR/"
done

# ---------------------------------------------------------------------------
# License texts — ship the component licenses plus a platform-specific README
# with the source-offer / upstream links. ONLY the components that exist on
# Windows are copied: the X11/xcb/xkbcommon family (libX11, libxkbcommon,
# xcb-util) is bundled solely by the Linux xcb platform plugin and is not
# present here.
# ---------------------------------------------------------------------------
#
# Shipping a binary WITHOUT these texts violates the GPL/LGPL, so a missing
# directory is a hard build failure -- never a silent skip.
#
# The README is a template: the component versions are substituted from what
# is actually bundled, so the source offer can never point at the wrong
# upstream tarball after a Qt bump.
echo "--- copying licenses:"
mkdir -p "$PORTABLE_DIR/licenses"

QT_SERIES=$(echo "$QT_VERSION" | cut -d. -f1,2)
FFMPEG_LIBS=$(ls "$DEPLOY_DIR"/av*.dll "$DEPLOY_DIR"/sw*.dll 2>/dev/null |
              xargs -n1 basename | sed 's/\.dll$//; s/-/ /' | sort | tr '\n' ',' | sed 's/,$//; s/,/, /g')
[ -n "$FFMPEG_LIBS" ] || { echo "ERROR: no bundled FFmpeg DLLs found" >&2; exit 1; }

sed -e "s/@QT_VERSION@/$QT_VERSION/g" \
    -e "s/@QT_SERIES@/$QT_SERIES/g" \
    -e "s/@FFMPEG_LIBS@/$FFMPEG_LIBS/g" \
    "$PROJECT/licenses/README-windows.txt.in" > "$PORTABLE_DIR/licenses/README.txt"
if grep -q '@[A-Z_]*@' "$PORTABLE_DIR/licenses/README.txt"; then
    echo "ERROR: unsubstituted placeholder in licenses/README.txt" >&2
    grep -n '@[A-Z_]*@' "$PORTABLE_DIR/licenses/README.txt" >&2
    exit 1
fi

# No ICU here: the MinGW Qt build does not bundle it (unlike the Linux one).
for sub in BlitzView Qt FFmpeg mingw-runtime; do
    if [ ! -d "$PROJECT/licenses/$sub" ]; then
        echo "ERROR: licenses/$sub is missing -- the package must not ship without it" >&2
        exit 1
    fi
    cp -r "$PROJECT/licenses/$sub" "$PORTABLE_DIR/licenses/"
done
find "$PORTABLE_DIR/licenses" -type f | sort

# ---------------------------------------------------------------------------
# Self-check: every DLL the executable imports must be present next to it or
# be a Windows system DLL. This is what catches a missing deployment before
# a user does.
# ---------------------------------------------------------------------------
missing=0
for dep in $(x86_64-w64-mingw32-objdump -p "$DEPLOY_DIR/blitzview.exe" \
             | sed -n 's/^\s*DLL Name: //p'); do
    case "$dep" in
        KERNEL32.dll|msvcrt.dll|SHELL32.dll|USER32.dll|GDI32.dll|ADVAPI32.dll|\
        ole32.dll|OLEAUT32.dll|WS2_32.dll|dbghelp.dll|VERSION.dll|COMDLG32.dll)
            continue ;;   # shipped with Windows
    esac
    if [ ! -f "$DEPLOY_DIR/$dep" ]; then
        echo "MISSING: $dep"
        missing=$((missing + 1))
    fi
done
[ "$missing" -eq 0 ] || { echo "deployment incomplete"; exit 1; }

# The launcher is linked -static, so it must import nothing but system DLLs.
# If that ever regresses it would need the DLLs it exists to hide.
for dep in $(x86_64-w64-mingw32-objdump -p "$PORTABLE_DIR/BlitzView.exe" \
             | sed -n 's/^\s*DLL Name: //p'); do
    case "$dep" in
        KERNEL32.dll|msvcrt.dll|SHELL32.dll|SHLWAPI.dll|USER32.dll|ADVAPI32.dll)
            continue ;;
    esac
    echo "launcher unexpectedly imports $dep — it must be self-contained"
    exit 1
done

echo "--- portable package: $PORTABLE_DIR"
du -sh "$PORTABLE_DIR"
ls "$PORTABLE_DIR"
echo "PORTABLE OK"
