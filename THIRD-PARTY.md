# Third-party components (F20 Bluetooth fork)

This is a fork of Rockbox that adds a Bluetooth A2DP **source** (plus AVRCP and
AAC) to the AIGO Eros Q Native / Surfans F20 (`erosqnative`, Ingenic X1000).
It bundles several third-party projects **vendored in-tree** so the repo builds
with a plain `git clone` + build — no separate fetch step. Each lives under its
own directory with its upstream license file preserved.

## ⚠️ Licensing — read this before redistributing or selling

> **BTstack (BlueKitchen) is free for open-source and personal/non-commercial
> use, but COMMERCIAL use requires a paid license from BlueKitchen.**
> https://bluekitchen-gmbh.com/

This is the one dependency with real teeth, and it has two consequences:

1. You may build, use, modify and share this firmware for personal and
   open-source purposes. You may **not** ship a product *for sale* built on it
   without obtaining a commercial BTstack license.
2. It is why this fork **cannot be merged into mainline Rockbox**: Rockbox is
   GPLv2, and BTstack's field-of-use restriction is incompatible with that.
   This fork is intentionally downstream-only.

Note also that Apache-2.0 (vo-aacenc, bluedroid) is generally considered
incompatible with GPLv2 for the purpose of relicensing the combined work as
GPLv2. In practice this is fine for a personal/open-source binary, but it is
another reason this tree is not upstreamable.

## Components

| Component | Path | License | Upstream |
|---|---|---|---|
| Rockbox | (whole tree) | GPLv2 | https://www.rockbox.org/ — `git.rockbox.org` |
| BTstack | `firmware/drivers/btstack/` | BlueKitchen license (free non-commercial; commercial = paid) | https://github.com/bluekitchen/btstack — **v1.6.2** |
| bluedroid SBC codec | `firmware/drivers/btstack/3rd-party/bluedroid/` | Apache-2.0 | (bundled with BTstack) |
| vo-aacenc (AAC-LC encoder) | `firmware/drivers/btstack/3rd-party/voaac/` | Apache-2.0 | https://github.com/mstorsjo/vo-aacenc — pinned commit in `vendor-voaac.sh` |
| FAAC (unused encoder path) | not committed | LGPL-2.1+ | https://github.com/knik0/faac — fetched by `vendor-faac.sh` |

## Updating the vendored dependencies

Each dependency has a vendoring script under
`firmware/target/mips/ingenic_x1000/erosqnative/` that pins a version and
records exactly what was staged and patched:

- `vendor-voaac.sh` — clones mstorsjo/vo-aacenc at a pinned commit, stages the
  encoder C path (no asm), applies bare-metal fixups (`__unused`, `NDEBUG`).
- `vendor-btstack.sh` — refreshes the curated BTstack subset to a pinned tag
  (`v1.6.2`). Re-test on hardware after any bump; BTstack APIs shift.
- `vendor-faac.sh` — the abandoned FAAC path (needs libm; superseded by
  vo-aacenc). Kept for reference only.

The vendored sources **are committed** so the repo is self-contained; the
scripts exist to *update* them, not as a required build step.
