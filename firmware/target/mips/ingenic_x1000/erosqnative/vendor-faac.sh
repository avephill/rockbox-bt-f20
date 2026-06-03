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
 * Replaces the autoconf-generated one. Must be included before any
 * FAAC header — vendor-faac.sh patches each .c file to do that.
 * It's also pulled by the libfaac internal headers that reference
 * MAX_CHANNELS so they don't depend on include ordering. */
#pragma once

/* X1000 has a single-precision hardware FPU; FAAC will use floats either
 * way, but FAAC_PRECISION_SINGLE selects the float code path explicitly. */
#define FAAC_PRECISION_SINGLE 1

/* No POSIX heap interaction beyond the malloc/free we already have in
 * Rockbox's app code (FAAC alloc happens at faacEncOpen-time, not in
 * the hot path). */
#ifndef HAVE_MALLOC
#define HAVE_MALLOC 1
#endif
#ifndef HAVE_FREE
#define HAVE_FREE   1
#endif

/* MAX_CHANNELS is normally set by meson/autoconf — the upstream default
 * is 64 ("max-channels" option). Stick with that since several internal
 * tables index against it. */
#ifndef MAX_CHANNELS
#define MAX_CHANNELS 64
#endif

/* Disable SSE / 3DNow code paths — MIPS doesn't have them. */
/* (No SSE2_ARCH define = portable path stays.) */

/* Math + stdio are used by FAAC's macros (FAAC_SQRT, FAAC_LOG10, etc.)
 * and error logging paths. Pulling them in here means every .c sees
 * the declarations without each one needing its own includes. */
#include <math.h>
#include <stdio.h>

/* Rockbox firmware has no stderr / POSIX file streams. FAAC's error
 * paths reference both; stub them out so the source compiles. These
 * defines are scoped to FAAC translation units only (no other Rockbox
 * code pulls FAAC's config.h). The error paths fire on misconfiguration,
 * which our wrapper guards against, so we accept the silent-on-error
 * trade for getting it to link.
 *
 * `do {} while(0)` keeps fprintf safe inside `if (cond) fprintf(...);`
 * without dangling-else surprises. */
#undef  stderr
#define stderr ((void*)0)
#undef  fprintf
#define fprintf(stream, ...) do { (void)(stream); } while(0)

/* Autoconf would set this from the FAAC release; supply a stub. The
 * value ends up as `libfaacName` in frame.c — for our purposes any
 * non-empty string works. */
#define PACKAGE_VERSION "1.30-rockbox"
EOF

# Provide a memory.h shim — older FAAC code uses the legacy <memory.h>
# header (mostly memcpy/memmove). Rockbox's libc has those in <string.h>;
# point the legacy include at it.
cat > "$DEST/libfaac/memory.h" <<'EOF'
/* Legacy header alias — FAAC includes <memory.h> for memcpy/memmove. */
#pragma once
#include <string.h>
EOF

# Patch every FAAC .c so config.h is the first thing the preprocessor
# sees. FAAC's headers reference MAX_CHANNELS (and rely on math.h being
# implicit via autoconf's CPPFLAGS); the autoconf build does this via
# -include, but Rockbox's flat make doesn't have per-directory CFLAGS,
# so we inject the include at vendor time instead.
for f in "$DEST/libfaac/"*.c; do
    if ! head -1 "$f" | grep -q 'config.h'; then
        tmpfile="$(mktemp)"
        { echo '#include "config.h"'; cat "$f"; } > "$tmpfile"
        mv "$tmpfile" "$f"
    fi
done

# Same for the headers that reference MAX_CHANNELS — without the include
# at their top, the .c-side config.h injection only helps the file that
# directly includes them, not transitive includes.
for f in "$DEST/libfaac/frame.h" "$DEST/libfaac/stereo.h" "$DEST/libfaac/util.h"; do
    if ! head -1 "$f" | grep -q 'config.h'; then
        tmpfile="$(mktemp)"
        { echo '#include "config.h"'; cat "$f"; } > "$tmpfile"
        mv "$tmpfile" "$f"
    fi
done

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
