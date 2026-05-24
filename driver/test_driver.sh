#!/usr/bin/env bash
set -euo pipefail

ROTATION_BIN=./test_rotation

fail() { echo "FAIL: $*" >&2; exit 1; }

# ── 1. Rotation unit test (no kernel headers required) ──────────────────────

echo "==> building rotation test"
gcc -std=c99 -Wall -Wextra -pedantic -O2 test_rotation.c -o "$ROTATION_BIN"

echo "==> running rotation test"
"$ROTATION_BIN" | tee /dev/stderr | grep -q "^0 passed" && fail "all rotation tests failed"
"$ROTATION_BIN" | grep -q "^[0-9]* passed, 0 failed"   || fail "rotation test reported failures"
echo "   rotation test passed"

# ── 2. Kernel module build (requires kernel headers) ────────────────────────

KDIR=/lib/modules/$(uname -r)/build

if [ ! -d "$KDIR" ]; then
    echo "==> SKIP: kernel headers not found at $KDIR"
    echo "==> all checks passed (rotation only)"
    exit 0
fi

echo "==> building kernel module (KDIR=$KDIR)"
make -s clean
make -s KDIR="$KDIR"
[ -f cirthfb.ko ] || fail "cirthfb.ko not produced after make"
echo "   cirthfb.ko built OK ($(du -h cirthfb.ko | cut -f1))"

make -s clean

echo "==> all checks passed"