# Rockbox for Surfans F20 — Bluetooth A2DP source + AAC

A fork of [Rockbox](https://www.rockbox.org/) that adds **Bluetooth audio
output** to the AIGO Eros Q Native / **Surfans F20** DAP (`erosqnative` target,
Ingenic X1000, soft-float MIPS32, BCM4343A1 radio). Stock Rockbox has no
Bluetooth on this player; this fork makes it a working A2DP **source** so you
can stream to wireless headphones and speakers.

## What it adds

- **A2DP source** (SBC) — stream Rockbox playback to any Bluetooth sink.
- **AAC-LC** via vo-aacenc, sent as **LATM** (what Apple H1 sinks expect).
  AAC is preferred when the sink offers it, with **automatic SBC fallback**
  for sinks that advertise AAC but refuse or ignore the configuration
  (some Xiaomi/Redmi speakers do exactly this).
- **RF-adaptive AAC bitrate** — under retransmit distress (body-shadowed
  outdoor links) the encoder steps 128 → 96 → 64 kbps mid-stream with no
  renegotiation, and recovers when the link is clean (with probe backoff so
  marginal links don't cycle). Ceiling set by the **Audio quality** setting.
- **Stall watchdog** — a retransmit-wedged link that freezes audio without
  disconnecting (observed: 21 s) is torn down and auto-reconnected in ~3 s.
- **AVRCP target** — play/pause/next/prev from the buds' buttons.
- A persistent BT service + menu (scan, connect, switch, forget, PIN pairing
  for legacy sinks) and a **BT Link Log** diagnostic with per-link TX power /
  RSSI / link-quality probes, dumped to `/.rockbox/bt_link.log` on pause and
  disconnect.

Non-Bluetooth extras that ride the same fork:

- **iPod-classic-style wheel acceleration** with a fast-scroll first-letter
  popup (both configurable: Settings → General → Display → Scrolling).
- **Clock sync over USB** — write `/.rockbox/settime.txt` containing
  `YYYY-MM-DD HH:MM:SS` while the player is mounted; the RTC is set the
  moment you unplug.

Engineering history, architecture notes, and 39 hard-won gotchas live in
[project-status.md](project-status.md).

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

Build the full install tree and unzip it over the mounted player (this keeps
codecs, language files, and the firmware in sync — don't copy `rockbox.erosq`
alone across versions):

```sh
make zip
unzip -o rockbox.zip -d /run/media/$USER/F20/    # or your mount point
sync
```

**One-time step for Bluetooth**: the BCM4343A1 radio needs its firmware
patch file on the card at `/.rockbox/BCM4343A1.hcd`. A copy ships at this
repo's root (see [THIRD-PARTY.md](THIRD-PARTY.md) for provenance):

```sh
cp BCM4343A1.hcd /run/media/$USER/F20/.rockbox/
```

Eject, then **fully power-cycle** the player (not just reboot) so the BT radio
re-initializes cleanly. Pair from Settings → Bluetooth.

## Tuning notes

- AAC bitrate is a runtime setting (Bluetooth menu → Audio quality:
  128/96/64 kbps) and acts as the *ceiling* for the adaptive-bitrate logic,
  which manages the live rate on its own.
- The vo-aacenc working arena is a fixed `VOAAC_ARENA_SIZE` (64 KB) in
  `bt-aac-encoder-voaac.c`; the `voaac arena N/M` log line reports live usage.
- Adaptive thresholds (distress gap 200 ms, 3-in-10 s trigger, 60 s recovery
  with backoff) live at the top of `bt-pcm-sink.c`; the walk-test data behind
  them is in project-status.md (C13/C13b).
