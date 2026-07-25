# Rockbox for the Surfans F20, with Bluetooth

A [Rockbox](https://www.rockbox.org/) fork that adds Bluetooth audio output
(A2DP source) to the AIGO Eros Q Native / Surfans F20 (`erosqnative` target,
Ingenic X1000, BCM4343A1 radio). Mainline Rockbox has no Bluetooth on this
player.

## Features

- Stream playback to Bluetooth headphones and speakers (SBC)
- AAC-LC when the sink supports it, with LATM framing (required by Apple H1
  buds). Sinks that advertise AAC but refuse it fall back to SBC
  automatically
- Bitrate adapts between 128/96/64 kbps with link quality, so a pocketed
  player outdoors degrades gracefully instead of dropping out. A watchdog
  reconnects links that wedge
- AVRCP: play/pause/next/prev from the headphone buttons
- Bonded device list, auto-connect on boot, PIN pairing for old sinks
- Link diagnostics logged to `/.rockbox/bt_link.log` (toggle in the
  Bluetooth menu)

Unrelated additions living in the same fork:

- iPod-style scroll wheel acceleration, with an optional first-letter popup
  while fast-scrolling long lists
- Clock set from a computer: write `YYYY-MM-DD HH:MM:SS` to
  `/.rockbox/settime.txt` while the player is mounted; applied on unplug

Design notes, tuning rationale, and the change history are in
[project-status.md](project-status.md).

## Install

Prebuilt firmware is on the [releases page](../../releases). The standard
Rockbox bootloader for this player must be installed first (see rockbox.org).
With the F20 mounted:

```sh
unzip -o rockbox.zip -d /run/media/$USER/F20/
cp BCM4343A1.hcd /run/media/$USER/F20/.rockbox/   # one time, needed for Bluetooth
sync
```

Fully power-cycle the player (not just a reboot), then pair from
Settings → Bluetooth.

## Building

Needs the Rockbox mipsel cross toolchain, built once with the upstream
helper:

```sh
tools/rockboxdev.sh    # pick 'm' (mips)
```

Then:

```sh
mkdir build-erosqnative && cd build-erosqnative
../tools/configure     # AIGO Eros Q Native, Normal build
make -j$(nproc) zip
```

Install `rockbox.zip` as above. Don't copy `rockbox.erosq` alone between
versions; the codecs and language files in the zip must match it.

All dependencies are vendored, so a plain clone builds. The `vendor-*.sh`
scripts under `firmware/target/mips/ingenic_x1000/erosqnative/` update the
vendored trees and are not part of the build.

## Licensing

Rockbox is GPLv2. This fork also bundles BTstack, which is free for
personal/non-commercial use only — so this tree can't be merged into
mainline Rockbox and can't be used in a product for sale. `BCM4343A1.hcd`
is proprietary Broadcom radio firmware extracted from the stock F20 image.
Details in [THIRD-PARTY.md](THIRD-PARTY.md).
