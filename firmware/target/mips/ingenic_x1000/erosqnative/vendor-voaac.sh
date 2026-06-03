#!/usr/bin/env bash
#
# Vendor mstorsjo/vo-aacenc (VisualOn AAC-LC encoder) under
# firmware/drivers/btstack/3rd-party/voaac/ so the erosqnative build can use
# it as the AAC encoder backend for A2DP source.
#
# Why vo-aacenc (vs FAAC / fdk-aac):
#   - FIXED-POINT integer math -> no libm dependency (FAAC is float and needs
#     sqrtf/powf/... which don't exist in the Rockbox firmware link), and fast
#     on the FPU-less, soft-float X1000.
#   - Plain C (fdk-aac is ~88 C++ files; Rockbox's build is C-only).
#   - Caller-supplied VO_MEM_OPERATOR -> no global malloc needed (the core
#     uses none either; only <string.h> from the system).
#   - adtsUsed=0 emits raw AAC access units, exactly what A2DP wants.
#   Apache-2.0 licensed.
#
# After this script succeeds:
#   1. Define BT_AAC_USE_VOAAC in firmware/export/config/erosqnative.h.
#   2. Rebuild and deploy. SBC and AAC are both advertised; sinks that prefer
#      AAC (Beats Fit Pro, AirPods) will pick it.
#
# Re-running is safe — it cleans the destination first.

set -euo pipefail

# Pinned for reproducibility. Bump only with a reason (fix / quality).
VOAAC_REPO="https://github.com/mstorsjo/vo-aacenc.git"
VOAAC_COMMIT="a277487e051e92e99a532294eed3c673f4d879f2"

# Resolve repo root from this script's location: firmware/target/mips/...
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
DEST="$REPO_ROOT/firmware/drivers/btstack/3rd-party/voaac"

if [ ! -d "$REPO_ROOT/firmware/drivers/btstack" ]; then
    echo "error: expected $REPO_ROOT to be the Rockbox repo root" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap "rm -rf '$TMP'" EXIT

echo "Cloning vo-aacenc..."
git -C "$TMP" clone --quiet "$VOAAC_REPO" voaac
git -C "$TMP/voaac" checkout --quiet "$VOAAC_COMMIT"

echo "Staging encoder source + headers..."
rm -rf "$DEST"
mkdir -p "$DEST/aacenc/src" "$DEST/aacenc/inc" "$DEST/aacenc/basic_op" \
         "$DEST/common/include"

# Encoder C sources — top-level only; deliberately EXCLUDE aacenc/src/asm/
# (ARMV5E/ARMV7 hand-asm). With ARMV5E / ARMV7Neon / ARM*_INASM left
# undefined, transform.c et al. compile their C reference implementations,
# so the asm is never referenced. MIPS uses the C path.
cp "$TMP"/voaac/aacenc/src/*.c        "$DEST/aacenc/src/"
cp "$TMP"/voaac/aacenc/inc/*.h        "$DEST/aacenc/inc/"
cp "$TMP"/voaac/aacenc/basic_op/*.c   "$DEST/aacenc/basic_op/"
cp "$TMP"/voaac/aacenc/basic_op/*.h   "$DEST/aacenc/basic_op/"
cp "$TMP"/voaac/common/include/*.h    "$DEST/common/include/"

# Bare-metal fixups (reproducible — re-applied on every vendor run):
#  1. __unused: vo-aacenc tags a couple of locals in tns.c with the bare
#     `__unused` attribute macro (from Android's <sys/cdefs.h>, absent here).
#     Map it to the plain GCC attribute.
find "$DEST" \( -name '*.c' -o -name '*.h' \) -print0 \
    | xargs -0 sed -i 's/\b__unused\b/__attribute__((unused))/g'
#  2. NDEBUG: an assert() in tns.c otherwise pulls __assert, which the Rockbox
#     firmware link doesn't provide. Define NDEBUG at the top of each source so
#     assert() compiles out (these are debug-only checks in shipping code).
for f in $(find "$DEST" -name '*.c'); do
    { echo '#define NDEBUG 1'; cat "$f"; } > "$f.tmp" && mv "$f.tmp" "$f"
done

# Provenance + license breadcrumb.
cp "$TMP"/voaac/COPYING "$DEST/COPYING" 2>/dev/null || true
cat > "$DEST/VENDOR.txt" <<EOF
Vendored from $VOAAC_REPO
Commit: $VOAAC_COMMIT
Subset: aacenc/{src,inc,basic_op} + common/include (encoder only, no asm).
Do NOT define ARMV5E / ARMV7Neon / ARM*_INASM — MIPS uses the C path.
Build deps: <string.h> only (no libm, no malloc; memory via VO_MEM_OPERATOR).
EOF

echo "Vendored files:"
( cd "$DEST" && find . -type f | sort | sed 's/^/  /' )
echo
echo "Done. Next: define BT_AAC_USE_VOAAC in erosqnative.h, then rebuild."
