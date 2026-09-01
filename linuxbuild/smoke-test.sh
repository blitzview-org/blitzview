#!/bin/sh
# Headless smoke test for the Linux portable package: starts BlitzView under
# Xvfb and expects it to keep running for the full timeout (exit 124/143)
# instead of crashing on startup. Runs on the HOST, not in the builder
# container -- the portable package is self-contained and targets glibc
# >= 2.28, which any CI runner or dev machine already provides.
#
# Usage: linuxbuild/smoke-test.sh [portable-dir]   (default: build/linux/BlitzView)
set -eu

PROJECT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PORTABLE_DIR=${1:-$PROJECT/build/linux/BlitzView}

[ -x "$PORTABLE_DIR/BlitzView" ] || { echo "no portable package at $PORTABLE_DIR"; exit 1; }

# Distributing the binaries without the GPL/LGPL license texts is a license
# violation, so the package is not shippable if any of them is missing. This
# check is what stops a rename in licenses/ from quietly emptying the package.
echo "--- checking bundled licenses"
[ -f "$PORTABLE_DIR/licenses/README.txt" ] || { echo "MISSING licenses/README.txt"; exit 1; }
if grep -q '@[A-Z_]*@' "$PORTABLE_DIR/licenses/README.txt"; then
    echo "licenses/README.txt still contains template placeholders"; exit 1
fi
for f in BlitzView/GPL-3.0.txt Qt/LGPL-3.0.txt Qt/THIRD-PARTY.txt FFmpeg/LGPL-2.1.txt \
         ICU/LICENSE xorg/COPYING libxkbcommon/LICENSE; do
    [ -s "$PORTABLE_DIR/licenses/$f" ] || { echo "MISSING licenses/$f"; exit 1; }
done
echo "licenses OK"

# Resolve the dynamic linking the way the launcher does, and report EVERY
# unresolved library at once. Without this, a missing system library shows up
# only as the loader's exit 127 naming the FIRST one it happens to hit -- fix
# that one, and the next run names the next. The package deliberately does not
# bundle system libraries (see the is_bundlable rule in deploy.sh), so the
# target system has to provide these.
echo "--- checking shared libraries"
missing=$(LD_LIBRARY_PATH="$PORTABLE_DIR/app/lib" ldd "$PORTABLE_DIR/app/blitzview" \
          | sed -n 's/^\s*\(.*\) => not found$/\1/p')
if [ -n "$missing" ]; then
    echo "MISSING system libraries:"
    echo "$missing" | sed 's/^/  /'
    exit 1
fi
echo "shared libraries OK"

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

echo "--- smoke test: $PORTABLE_DIR/BlitzView (DISPLAY=$DISPLAY, own-xvfb=$OWN_XVFB)"
set +e
timeout 15 "$PORTABLE_DIR/BlitzView"
status=$?
set -e

case "$status" in
    124|143) echo "SMOKE TEST OK (ran for the full duration)" ;;
    *)       echo "SMOKE TEST FAILED: exited early with status $status"; exit 1 ;;
esac
