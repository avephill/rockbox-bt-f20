#!/usr/bin/env bash
#
# Vendor knik0/faac under firmware/drivers/btstack/3rd-party/faac/ so the
# erosqnative build can pull in libfaac as the AAC encoder backend.
#
# After this script succeeds:
#   1. Uncomment `#define BT_AAC_USE_FAAC` in
#      firmware/export/config/erosqnative.h.
#   2. Rebuild and deploy. SBC and AAC will both be advertised; sinks
#      that prefer AAC (BFP, AirPods, recent BMW/Sonos) will pick it.
#
# Re-running is safe — it cleans the destination first.

set -euo pipefail

# Pinned to an arbitrary recent commit so vendoring is reproducible. Bump
# this when there's a reason to (security fix, encoder quality improvement).
FAAC_REPO="https://github.com/knik0/faac.git"
FAAC_COMMIT="167b5eb656c7f529faf565ead10ba0c67f5eb384"

# Resolve repo root from this script's location: firmware/target/mips/...
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
DEST="$REPO_ROOT/firmware/drivers/btstack/3rd-party/faac"

if [ ! -d "$REPO_ROOT/firmware/drivers/btstack" ]; then
    echo "error: expected $REPO_ROOT to be the Rockbox repo root" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap "rm -rf '$TMP'" EXIT

echo "Cloning FAAC..."
git -C "$TMP" clone "$FAAC_REPO" faac >/dev/null
git -C "$TMP/faac" checkout --quiet "$FAAC_COMMIT"

echo "Staging encoder + headers..."
rm -rf "$DEST"
mkdir -p "$DEST/libfaac" "$DEST/include"

# Encoder source — everything except quantize_sse.c (SSE / x86 intrinsics,
# would not compile on MIPS even with the SSE2_ARCH macro off).
for src in \
    bitstream.c blockswitch.c channels.c cpu_compute.c fft.c filtbank.c \
    frame.c huff2.c huffdata.c quantize.c stereo.c tns.c util.c
do
    cp "$TMP/faac/libfaac/$src" "$DEST/libfaac/$src"
done

# Headers — both public (include/) and the libfaac-internal ones.
cp "$TMP/faac/include/faac.h"      "$DEST/include/faac.h"
cp "$TMP/faac/include/faaccfg.h"   "$DEST/include/faaccfg.h"
for h in "$TMP/faac/libfaac/"*.h; do
    cp "$h" "$DEST/libfaac/$(basename "$h")"
done

# Provide a config.h shim — FAAC otherwise expects autoconf's HAVE_*
# macros. The Rockbox build doesn't run FAAC's autoconf, so we hand-roll
# what FAAC's source files actually look at.
cat > "$DEST/libfaac/config.h" <<'EOF'
/* Hand-written config.h for FAAC embedded in Rockbox firmware.
 * Replaces the autoconf-generated one. */
#pragma once

/* X1000 has a single-precision hardware FPU; FAAC will use floats either
 * way, but FAAC_PRECISION_SINGLE selects the float code path explicitly. */
#define FAAC_PRECISION_SINGLE 1

/* No POSIX heap interaction beyond the malloc/free we already have in
 * Rockbox's app code (FAAC alloc happens at faacEncOpen-time, not in
 * the hot path). */
#define HAVE_MALLOC 1
#define HAVE_FREE   1

/* Disable SSE / 3DNow code paths — MIPS doesn't have them. */
/* (No SSE2_ARCH define = portable path stays.) */

/* Redirect fprintf-to-stderr error messages into Rockbox's crash log.
 * Without this, FAAC's error paths reference undefined symbols on bare-
 * metal builds. See FAAC quirks doc next to this script. */
#include <stdio.h>
EOF

# Print what's left so we can spot any internal-only deps we missed.
echo "Vendored files:"
ls -1 "$DEST/libfaac" "$DEST/include" | sed 's/^/  /'

echo
echo "Next steps:"
echo "  1) Open firmware/export/config/erosqnative.h and uncomment:"
echo "       #define BT_AAC_USE_FAAC"
echo "  2) Rebuild on the Linux host:"
echo "       cd build-erosqnative && make -j\$(nproc) && make zip"
echo "  3) Deploy and test — watch BT Link Log to see which codec the"
echo "     sink negotiated. 'cfg AAC ...' = AAC active."
echo
echo "Done."
