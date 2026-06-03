# Rockbox for Surfans F20 — Bluetooth A2DP source + AAC

A fork of [Rockbox](https://www.rockbox.org/) that adds **Bluetooth audio
output** to the AIGO Eros Q Native / **Surfans F20** DAP (`erosqnative` target,
Ingenic X1000, soft-float MIPS32, BCM4343A1 radio). Stock Rockbox has no
Bluetooth on this player; this fork makes it a working A2DP **source** so you
can stream to wireless headphones and speakers.

## What it adds

- **A2DP source** (SBC) — stream Rockbox playback to any Bluetooth sink.
- **AAC-LC** via vo-aacenc, sent as **LATM** (what Apple H1 sinks expect).
  AAC is preferred when the sink offers it; SBC is the fallback. On Beats Fit
  Pro this gives noticeably less stutter while walking than SBC, because the
  buds buffer AAC deeply enough to ride through RF retransmit stalls.
- **AVRCP target** — play/pause/next/prev from the buds' buttons.
- A persistent BT service + menu (scan, connect, switch, forget) and an
  on-screen **BT Link Log** diagnostic (also dumped to `/.rockbox/bt_link.log`
  on disconnect).

This fork is **downstream-only** — it cannot be merged into mainline Rockbox
for licensing reasons (see [THIRD-PARTY.md](THIRD-PARTY.md)).

## Dependencies

All third-party code is **vendored in-tree** — a plain clone builds with no
extra fetch step. See [THIRD-PARTY.md](THIRD-PARTY.md) for the full list and
**important licensing notes** (notably: BTstack is free for non-commercial use
only). The `vendor-*.sh` scripts under
`firmware/target/mips/ingenic_x1000/erosqnative/` exist to *update* those
dependencies, not as a build prerequisite.

## Building

You need the Rockbox **mipsel cross toolchain**. Build it once with the
upstream helper (installs to `~/.rockbox-toolchain` or `/usr/local`):

```sh
tools/rockboxdev.sh        # select the 'm' (mips) target; needs build deps
export PATH=$PATH:/usr/local/bin   # wherever it installed mipsel-elf-gcc
```

Then build the player firmware:

```sh
mkdir build-erosqnative && cd build-erosqnative
../tools/configure        # choose: AIGO Eros Q Native (erosqnative), Normal build
make -j$(nproc)
```

This produces `rockbox.erosq` (the scrambled firmware) and the `.rockbox/`
tree. AAC is enabled by default via `#define BT_AAC_USE_VOAAC` in
`firmware/export/config/erosqnative.h`.

> First build also needs the bootloader flashed once (standard Rockbox
> erosqnative install). If you already run Rockbox on the F20, you only need to
> replace the firmware below.

## Installing

Mount the F20 as USB mass storage and copy the firmware in:

```sh
cp rockbox.erosq /Volumes/F20/.rockbox/rockbox.erosq    # macOS
# or your mount point on Linux
sync
```

Eject, then **fully power-cycle** the player (not just reboot) so the BT radio
re-initializes cleanly. Pair from Settings → Bluetooth.

## Tuning notes

- AAC bitrate: `AAC_TARGET_BITRATE` in `bt-pcm-sink.c`'s caller
  (`bt-service.c`). 128 kbps is transparent and low-airtime; raise toward
  256 kbps for quality if your link has headroom.
- The vo-aacenc working arena is a fixed `VOAAC_ARENA_SIZE` (64 KB) in
  `bt-aac-encoder-voaac.c`; the `voaac arena N/M` log line reports live usage.
