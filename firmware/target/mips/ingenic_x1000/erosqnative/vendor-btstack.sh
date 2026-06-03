#!/usr/bin/env bash
#
# Re-vendor / update BTstack (BlueKitchen) under firmware/drivers/btstack/.
#
# Unlike vendor-voaac.sh / vendor-faac.sh, BTstack was originally imported by
# hand as a curated SUBSET (specific .c files + all headers + the embedded
# platform glue) — see the "vendor BTstack" commits. This script does NOT
# re-derive that subset; it REFRESHES the files already present in the tree to
# a pinned upstream version. That makes minor version bumps (bug fixes) a
# one-command operation while preserving the exact file set we compile.
#
# Scope / limitations:
#   - Only files that already exist under src/ and platform/ are refreshed.
#   - btstack_config.h (our Rockbox-specific config) is preserved untouched.
#   - 3rd-party/ (bluedroid SBC, voaac) is preserved — those are vendored
#     separately by their own scripts.
#   - A MAJOR version bump that adds/renames files still needs manual work:
#     add the new .c to firmware/SOURCES and re-run. Files upstream removed
#     are reported as warnings (resolve by hand).
#
# After running: rebuild and hardware-test. BTstack's A2DP/AVDTP/AVRCP APIs do
# shift between versions, so check bt-service.c / bt-pcm-sink.c still compile
# and that streaming + the AAC LATM path still work.
#
#   IMPORTANT: this is BlueKitchen BTstack. It is free for open-source and
#   non-commercial use; COMMERCIAL use requires a paid license from
#   BlueKitchen. See THIRD-PARTY.md. This is the dependency that makes the
#   whole fork un-upstreamable into mainline (GPLv2) Rockbox.

set -euo pipefail

# Pinned for reproducibility. Bump only deliberately (and retest on hardware).
# Current in-tree version: see firmware/drivers/btstack/src/btstack_version.h
BTSTACK_REPO="https://github.com/bluekitchen/btstack.git"
BTSTACK_REF="v1.6.2"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
DEST="$REPO_ROOT/firmware/drivers/btstack"

if [ ! -d "$DEST/src" ]; then
    echo "error: $DEST/src not found — expected an existing BTstack tree to refresh" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap "rm -rf '$TMP'" EXIT

echo "Cloning BTstack $BTSTACK_REF ..."
git -C "$TMP" clone --quiet --depth 1 --branch "$BTSTACK_REF" "$BTSTACK_REPO" btstack

echo "Refreshing curated subset (src/ + platform/, preserving btstack_config.h + 3rd-party/) ..."
refreshed=0
missing=0
( cd "$DEST" && find src platform -type f \( -name '*.c' -o -name '*.h' \) ) \
| while read -r f; do
    if [ -f "$TMP/btstack/$f" ]; then
        cp "$TMP/btstack/$f" "$DEST/$f"
        refreshed=$((refreshed+1))
    else
        echo "  WARN: $f no longer present upstream (renamed/removed) — resolve by hand"
        missing=$((missing+1))
    fi
done

echo "Done. Verify firmware/drivers/btstack/src/btstack_version.h, rebuild, and"
echo "hardware-test A2DP streaming (incl. the AAC LATM path) before committing."
