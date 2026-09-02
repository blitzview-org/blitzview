#!/bin/sh
# Smoke test for the macOS portable package: starts BlitzView.app and expects
# it to still be running after a timeout, instead of crashing on startup.
# Runs on the HOST -- macOS CI runners have a live WindowServer, so unlike
# the Linux/Windows smoke tests there is no Xvfb equivalent to start.
#
# No `timeout` binary is assumed (BSD userland does not ship GNU coreutils by
# default): the timeout is done by hand with a background process + sleep.
#
# Usage: macbuild/smoke-test.sh [portable-dir]   (default: build/macos/BlitzView)
set -eu

PROJECT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PORTABLE_DIR=${1:-$PROJECT/build/macos/BlitzView}
APP="$PORTABLE_DIR/BlitzView.app"
EXE="$APP/Contents/MacOS/BlitzView"

[ -x "$EXE" ] || { echo "no portable package at $PORTABLE_DIR"; exit 1; }

# Distributing the binaries without the GPL/LGPL license texts is a license
# violation, so the package is not shippable if any of them is missing. This
# check is what stops a rename in licenses/ from quietly emptying the package.
# licenses/ sits next to BlitzView.app (not inside Contents/Resources), same
# top-level shape as the Linux/Windows portable packages.
echo "--- checking bundled licenses"
[ -f "$PORTABLE_DIR/licenses/README.txt" ] || { echo "MISSING licenses/README.txt"; exit 1; }
if grep -q '@[A-Z_]*@' "$PORTABLE_DIR/licenses/README.txt"; then
    echo "licenses/README.txt still contains template placeholders"; exit 1
fi
for f in BlitzView/GPL-3.0.txt Qt/LGPL-3.0.txt Qt/THIRD-PARTY.txt FFmpeg/LGPL-2.1.txt; do
    [ -s "$PORTABLE_DIR/licenses/$f" ] || { echo "MISSING licenses/$f"; exit 1; }
done
echo "licenses OK"

echo "--- smoke test: $EXE"
"$EXE" &
PID=$!
sleep 15

if kill -0 "$PID" 2>/dev/null; then
    echo "SMOKE TEST OK (still running after the full duration)"
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
else
    set +e
    wait "$PID"
    status=$?
    set -e
    echo "SMOKE TEST FAILED: exited early with status $status"
    exit 1
fi
