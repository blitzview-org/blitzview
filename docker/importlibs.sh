#!/bin/sh
# Runs at IMAGE BUILD time, after deps.sh.
#
# Qt Multimedia ships FFmpeg DLLs but no import libraries and no headers — it
# never expected anyone but Qt itself to link against them. BlitzView does, so
# that the package contains exactly one set of FFmpeg codecs instead of two
# (the alternative, shipping our own, would add ~50-100 MB of duplicate
# codecs for functionality already present).
#
# The export table is read from the DLL with objdump (there is no gendef on
# AlmaLinux 8, and the AmanoTeam toolchain does not ship one), then dlltool
# turns the extracted names into a MinGW import library. Both run natively on
# Linux — no Wine needed, they only parse PE files.
set -eux

. /usr/src/deps.env

QT_BIN="/opt/qt/$QT_VERSION/mingw_64/bin"
OUT="/opt/ffmpeg/lib"
mkdir -p "$OUT"
cd "$(mktemp -d)"

for lib in avcodec avformat avutil swscale swresample; do
    # Qt's DLLs carry the FFmpeg soname major: avcodec-61.dll, avutil-59.dll, ...
    dll=$(ls "$QT_BIN/$lib"-*.dll 2>/dev/null)
    [ -n "$dll" ] || { echo "SKIP: no $lib DLL found"; continue; }
    dllname=$(basename "$dll")

    # Extract the exported symbol names. objdump prints the export table as
    # "[Ordinal/Name Pointer] Table"; symbol lines are the ones carrying a
    # "+base[...]" field, and the symbol name is the last whitespace token.
    {
        echo "EXPORTS"
        x86_64-w64-mingw32-objdump -p "$dll" \
            | sed -n '/\[Ordinal\/Name Pointer\] Table/,$p' \
            | awk '/\+base\[/ {print $NF}'
    } > "$lib.def"

    # -D records the DLL name the executable will import at runtime, which
    # must be the versioned one, not "libavcodec.dll".
    x86_64-w64-mingw32-dlltool \
        --input-def "$lib.def" \
        --dllname "$dllname" \
        --output-lib "$OUT/lib$lib.dll.a"

    echo "import lib: lib$lib.dll.a -> $dllname"
done

ls -la "$OUT"
