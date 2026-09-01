#!/bin/sh
# Headless smoke test for the Windows portable package: starts BlitzView.exe
# under Wine + Xvfb and expects it to keep running for the full timeout
# (exit 124/143) instead of crashing on startup. Runs on the HOST (needs
# `wine` and `xvfb` installed), not in the builder container -- the
# container has Wine but no X server.
#
# Usage: winbuild/smoke-test.sh [portable-dir]   (default: build/windows/BlitzView)
set -eu

PROJECT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PORTABLE_DIR=${1:-$PROJECT/build/windows/BlitzView}

[ -f "$PORTABLE_DIR/BlitzView.exe" ] || { echo "no portable package at $PORTABLE_DIR"; exit 1; }

# Distributing the binaries without the GPL/LGPL license texts is a license
# violation, so the package is not shippable if any of them is missing. This
# check is what stops a rename in licenses/ from quietly emptying the package.
echo "--- checking bundled licenses"
[ -f "$PORTABLE_DIR/licenses/README.txt" ] || { echo "MISSING licenses/README.txt"; exit 1; }
if grep -q '@[A-Z_]*@' "$PORTABLE_DIR/licenses/README.txt"; then
    echo "licenses/README.txt still contains template placeholders"; exit 1
fi
for f in BlitzView/GPL-3.0.txt Qt/LGPL-3.0.txt Qt/THIRD-PARTY.txt FFmpeg/LGPL-2.1.txt \
         mingw-runtime/GCC-RUNTIME-LIBRARY-EXCEPTION-3.1.txt \
         mingw-runtime/MINGW-W64-RUNTIME-COPYING.txt \
         mingw-runtime/MIT-winpthreads.txt; do
    [ -s "$PORTABLE_DIR/licenses/$f" ] || { echo "MISSING licenses/$f"; exit 1; }
done
echo "licenses OK"

OWN_XVFB=0
if [ -z "${DISPLAY:-}" ]; then
    Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    trap 'kill $XVFB_PID 2>/dev/null || true' EXIT
    DISPLAY=:99
    OWN_XVFB=1
    sleep 1
fi
export DISPLAY

echo "--- smoke test: $PORTABLE_DIR/BlitzView.exe (DISPLAY=$DISPLAY, own-xvfb=$OWN_XVFB)"
set +e
( cd "$PORTABLE_DIR" && timeout 15 wine BlitzView.exe )
status=$?
set -e

case "$status" in
    124|143) echo "SMOKE TEST OK (ran for the full duration)" ;;
    *)       echo "SMOKE TEST FAILED: exited early with status $status"; exit 1 ;;
esac
