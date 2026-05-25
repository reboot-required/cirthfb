#!/usr/bin/env bash
set -euo pipefail

DAEMON=./cirthfbd
RENDER_TEST_BIN=./render_test
FAKE_FB=/tmp/fake_fb_$$
FB_SIZE=3904   # ceil(250/8) * 122 = 32 * 122

cleanup() {
    rm -f "$FAKE_FB"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

# ── 1. Build ────────────────────────────────────────────────────────────────

echo "==> building daemon"
make -s clean
make -s

echo "==> building render_test"
gcc -std=c99 -Wall -Wextra -pedantic -O2 -DRENDER_TEST render.c -o "$RENDER_TEST_BIN" -I.

# ── 2. Render test ──────────────────────────────────────────────────────────

echo "==> render_test: checking PBM output"
output=$("$RENDER_TEST_BIN" | file -)
echo "   $output"
echo "$output" | grep -q "Netpbm image data, size = 250 x 122" \
    || fail "render_test did not produce a 250x122 PBM"

# ── 3. Full daemon run against fake framebuffer ─────────────────────────────

echo "==> creating fake framebuffer ($FB_SIZE bytes)"
dd if=/dev/zero of="$FAKE_FB" bs="$FB_SIZE" count=1 2>/dev/null

echo "==> running daemon (UPDATE_INTERVAL=5, FB_DEV=$FAKE_FB)"
UPDATE_INTERVAL=5 FB_DEV="$FAKE_FB" "$DAEMON" &
DAEMON_PID=$!

# Wait for one metrics cycle (cpu_percent sleeps 1s internally)
sleep 2.5

echo "==> sending SIGTERM"
kill "$DAEMON_PID"
wait "$DAEMON_PID" 2>/dev/null && rc=0 || rc=$?

# SIGTERM exit: the signal causes exit(0) via the loop condition; the shell
# may still report 143 if the process was killed before wait() saw it.
[ "$rc" -eq 0 ] || [ "$rc" -eq 143 ] \
    || fail "daemon exited with unexpected status $rc"

# ── 4. Verify framebuffer was written ──────────────────────────────────────

echo "==> verifying framebuffer contents"

actual_size=$(wc -c < "$FAKE_FB")
[ "$actual_size" -eq "$FB_SIZE" ] \
    || fail "framebuffer size changed: expected $FB_SIZE, got $actual_size"

# The daemon calls render_clear() (fills 0xFF) then draws text (clears bits to 0).
# After a successful render there must be at least one 0x00 byte somewhere.
python3 - "$FAKE_FB" <<'EOF'
import sys

with open(sys.argv[1], 'rb') as f:
    data = f.read()

if all(b == 0xFF for b in data):
    print("FAIL: framebuffer is blank (all 0xFF) — daemon did not render", file=sys.stderr)
    sys.exit(1)

black_pixels = sum(bin(b).count('0') - 1 for b in data)  # -1 for '0b' prefix
print(f"   {black_pixels} black pixels rendered")
EOF

# ── 5. Check the four expected text rows are non-blank ──────────────────────
#
# draw_string() places lines at y = 4, 20, 36, 52.
# Each row in the 1bpp buffer is FB_WIDTH/8 = 31 bytes.

echo "==> verifying four text rows (y=4,20,36,52)"

python3 - "$FAKE_FB" <<'EOF'
import sys

ROW_BYTES = 32   # ceil(250/8) — matches FB_STRIDE
FONT_H    = 8
TEXT_ROWS = [4, 20, 36, 52]

with open(sys.argv[1], 'rb') as f:
    data = f.read()

for y in TEXT_ROWS:
    block_start = y * ROW_BYTES
    block = data[block_start: block_start + FONT_H * ROW_BYTES]
    if all(b == 0xFF for b in block):
        print(f"FAIL: text block y={y}..{y+FONT_H-1} is blank", file=sys.stderr)
        sys.exit(1)
    print(f"   y={y:2d}: ok")
EOF

echo "==> all checks passed"
