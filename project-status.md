# Rockbox erosqnative — Bluetooth A2DP Source

**Last updated**: 2026-07-22 (C13 outdoor RF robustness; non-BT: wheel acceleration + letter overlay, RTC sync via settime.txt)
**Branch**: `bt-aac`
**Target**: Surfans F20 DAP (Rockbox `erosqnative`, Ingenic X1000 SoC, MIPS32 bare-metal, HW4 revision)
**Combo chip**: BCM4343A1 (WiFi + Bluetooth) — only BT brought up.

## What works today

End-to-end Bluetooth audio at full quality: scan for nearby devices, pick one, pair (Just Works SSP), open an A2DP source stream, and play music files from Rockbox's audio engine through the BT speaker — smooth, no stuttering. The last-connected device is remembered across reboots. The BTstack run loop runs in its own kernel thread so the playback engine and UI thread share the CPU correctly.

| Capability | Status |
|------------|--------|
| HCI transport over UART (115k → 3M) | working |
| BCM4343A1 patchram firmware upload | working |
| HCI inquiry (device discovery) | working |
| SSP pairing (`NO_INPUT_NO_OUTPUT`, Just Works) | working |
| A2DP source profile + AVDTP signaling | working |
| SBC encoding (bluedroid) | working |
| Audio streaming (smooth, real-time) | working |
| Device picker UI + persistent last-connected | working |
| Sink swap (route Rockbox PCM → BT instead of DAC) | working |
| Jack-detect override (no headphones required for playback) | working |
| Dedicated BT kernel thread (run loop off foreground) | working (C6b) |
| Multi-device bonded list (up to 8) + forget UI | working (C9) |
| Auto-connect on boot (Settings toggle) | working (C10) |
| AVRCP target (sink button play/pause/next/prev) | working (C11) |
| AAC-LC source codec via vo-aacenc, LATM framing | working (C12) |
| AAC bitrate setting (128/96/64 kbps) | working (C12) |
| AAC→SBC fallback for sinks with unusable/refused AAC | working per user report 2026-07-22 (C12b firmware verified on hardware; `sel SBC` itself still never observed in a log) |
| Link-event logging to /.rockbox/bt_link.log (toggle) | working (C12) |
| Adaptive AAC bitrate (downshift on RF distress, auto-recover) | implemented 2026-07-22 (C13), needs outdoor-walk verification |
| Link probe: TX power + RSSI + link quality every 4 s | implemented 2026-07-22 (C13) |
| Stall watchdog (frozen link → auto reconnect) | implemented 2026-07-22 (C13), needs outdoor-walk verification |

Where it lives in the UI:
- **Front-page `Bluetooth`** entry on the root menu — primary path. The parent menu shows **Devices / Audio quality / Link logging / Auto-connect / Turn Bluetooth off**, each with its value inline (DYNTEXT items routing through `option_screen` for persistence).
- **Settings → Bluetooth** — same menu.
- **Debug → Bluetooth A2DP** — legacy debug viewer (also a thin client of `bt-service`).

Buttons inside the Devices screen:
- **PLAY** — connect to highlighted device, switch to it if something else is streaming, or disconnect if it's the active one.
- **scroll wheel** — navigate the list (scan results when scanning has produced any, otherwise the bonded list).
- **MENU** (tap or hold) — opens an explicit actions menu: *Scan for new devices* / *Forget this device* (forget with yes/no confirm). Replaced the old tap-vs-hold gesture, which misfired scans on short holds.
- **BACK** — leave the screen (BT keeps running so audio continues).

Pairing additional devices: open the BT menu → tap **MENU** to scan → pick the new device with **PLAY** → it streams + gets added to `bt_bonded.dat`. The bonded list is MRU-ordered, so the most recently used device is what auto-connect picks at boot. A2DP source streams to one sink at a time — pairing more devices means "remembered for later switching", not simultaneous output.

## Roadmap

| Step | Description |
|------|-------------|
| ~~C6b~~ | ~~Move BTstack run loop off the foreground onto a kernel thread.~~ **Done 2026-04-26.** |
| ~~C7~~ | ~~Persist link keys to `/.rockbox/` via BTstack TLV.~~ **Done 2026-04-26** — file-backed TLV at `/.rockbox/bt_keys.dat`, verified reconnect across power cycle. |
| ~~C8~~ | ~~Promote BT out of the debug menu.~~ **Done 2026-04-27** — `bt-service.c` is a persistent background service (BT thread runs across screens, audio keeps playing while you navigate). Reachable from root menu and Settings → General → Bluetooth via `bluetooth_menu.c`. The old `Debug → Bluetooth A2DP` screen still exists as a parallel viewer. |
| ~~C9~~ | ~~Multi-device list + forget.~~ **Done 2026-04-27** — `bt_bonded.dat` (magic `BTB1`) replaces `bt_last.dat` with a MRU-ordered list of up to 8 bonded devices. Auto-migrates from `bt_last.dat` on first run. Bonded list is shown in `bt_open_screen` whenever no scan results are present; **long-press MENU** on a highlighted entry → yes/no confirm → `gap_drop_link_key_for_bd_addr` + remove from list. New service API: `bt_service_get_bonded`, `bt_service_forget`. |
| ~~C10~~ | ~~Auto-enable BT on boot.~~ **Done 2026-04-27** — `global_settings.bt_autoconnect` (Settings → Bluetooth → "Auto-connect Bluetooth on boot") + `main.c` calls `bt_service_enable_and_connect_last()` after `validate_start_directory_init()`. Non-blocking: arms a flag in bt-service and the BT thread auto-issues the connect once HCI hits WORKING (~3-5 s into boot). |
| ~~C10b~~ | ~~Audio-quality hardening for Apple H1-class sinks (AirPods / Beats Fit Pro).~~ **Done 2026-04-29** — two changes landed and stuck: (1) SBC `max_bitpool` advertised cap dropped 53 → 35 (smaller on-air frames, more retransmit headroom); (2) `gap_set_default_link_policy_settings(LM_LINK_POLICY_DISABLE_ALL_LM_MODES)` so the local LM refuses sniff/hold/park requests from the peer. A third — `gap_enable_link_watchdog(30)` for HCI auto-flush — was tried and reverted; re-tried at 50 ms in C10c and reverted again because it caused the Beats Fit Pro to disconnect ~5 s after every connect. Bose / Redmi unaffected throughout. |
| ~~C10c~~ | ~~BR-only ACL packet types — body-blocking robustness for Beats Fit Pro.~~ **Done 2026-05-03** — `hci_enable_acl_packet_types(ACL_PACKET_TYPES_BR)` in bt-service before HCI bring-up. EDR (2-DH*/3-DH*) needs ~5-9 dB more SNR than basic-rate to stay below threshold; in a body-blocking null (F20 in breast pocket, head turned to put the skull between source and the primary Beats bud) that's exactly the margin lost, and EDR drops into a retransmit cascade audible as a multi-hundred-ms tear or sustained dropout. BR rides through the same null with frame loss instead of cascade collapse. Result: head-turn cutouts went from "every few seconds while walking" to "rare and short" with the F20 in a breast pocket. Pants-pocket dropouts ("every few minutes") persist — 50+ cm of body tissue between source and primary bud is a hard physical limit that controller knobs can't fully fix. Bandwidth is fine: SBC bitpool 35 stereo is ~250 kbps payload, 1-DH5 carries ~700 kbps usable. Bose / Redmi sinks unaffected. |
| ~~C11 v1~~ | ~~AVRCP target — play / pause / next / prev from sink button.~~ **Done 2026-04-29, completed 2026-05-03** — vendored `avrcp.c`, `avrcp_target.c`, `avrcp_controller.c` from BTstack master. bt-service inits both target+controller halves, registers AVRCP target/controller SDP records, and maps `AVRCP_SUBEVENT_OPERATION` op_ids → playback API. PASSTHROUGH PRESS-only (RELEASE ignored to avoid double-fire). Two follow-ups landed 2026-05-03 to make it actually work on Beats Fit Pro: (1) `MAX_NR_AVRCP_CONNECTIONS` bumped 1→2 because `avrcp_connect` allocates separate slots for CONTROLLER + TARGET roles on the same peer (otherwise rc=0x56 `BTSTACK_MEMORY_ALLOC_FAILED`); (2) source-side `avrcp_connect()` issued on `A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED` because BFP never initiates AVCTP from the sink side; (3) PLAY (0x44) and PAUSE (0x46) ops both toggle against `audio_status()` rather than mapping literally — without playback-status notifications BFP only ever sends PAUSE so a literal map would only work for the first tap. |
| C11 v2 | **AVRCP target follow-ups (optional polish).** (a) Playback-status notifications back to the sink (`avrcp_target_set_playback_status` + `support_event(NOTIFICATION_PLAYBACK_STATUS_CHANGED)`) so non-Apple controllers see proper PLAY/PAUSE alternation. Not needed for BFP since the toggle in v1 handles it; (b) AVRCP 1.4 metadata (`GetElementAttributes`) — only earns its weight on sinks with displays or car-display readouts; (c) absolute volume sync (bud volume up/down adjusts source volume). All independent of v1. |
| ~~C12~~ | ~~AAC source codec.~~ **Done 2026-06-08** — AAC-LC via vo-aacenc (fixed-point, static 64 KB arena, no libm/malloc), LATM(MCP1) framing (gotcha 29), explicit codec selection preferring AAC with SBC fallback, 128/96/64 kbps "Audio quality" setting, DH3 slot cap + stay-master, link-event logging to `/.rockbox/bt_link.log`, read-only TX-power probe (result: pinned at 12 dBm ceiling — frame size is the only remaining robustness lever). Verified on Beats Fit Pro: cleaner than SBC while walking. See "What changed since 2026-05-03". |
| ~~C12b~~ | ~~AAC negotiation hardening.~~ **Done 2026-07-14** — diagnosed the Redmi "shows playing but silent" bug from the on-device link log and fixed the negotiation layer: sink-caps intersection before choosing AAC, `A2DP_SUBEVENT_COMMAND_REJECTED` handling + 4 s config watchdog with disconnect-reconnect SBC fallback, AAC encoder rebuild on resume-after-suspend, oversize-frame-vs-MTU drop+log, 44.1-only AAC caps. Gotchas 29–35. **Needs on-hardware verification against the Redmi** (expected: `AAC caps …` + either AAC working or `sel SBC` in the log). |
| ~~C13~~ | ~~Outdoor RF robustness.~~ **Done 2026-07-22** — analysis of the outdoor-walk link log (constant 60–200 ms send stalls, `slots max=1` LM downgrades, one 21 s frozen stretch with the ACL alive) + hardware research concluded the F20 side is the RF bottleneck (BCM4343A1 combo chip, single shared antenna, generic module patchram; TX pinned at its 12 dBm ceiling — confirmed live every probe round) and outdoors lacks the indoor multipath that fills body-shadow nulls. Three levers landed, one commit each: (1) **adaptive AAC bitrate** — 3 send gaps >200 ms in 10 s drop the encode rate a ladder step (128→96→64 k) mid-stream with no renegotiation (gotcha 36), 60 s clean recovers one step; (2) **link probe** — the txpow probe now chains Read_RSSI + Read_Link_Quality, logging `link tx=12/12 rssi=R lq=Q` (gotcha 38); (3) **stall watchdog** — ≥5 s with no successful media send → pending-switch + `gap_disconnect` → auto-reconnect (gotcha 37), rate-limited to one kick/30 s. Gotchas 36–38. |
| next | **Outdoor walk verification** — same conditions as the mid-July outdoor log, Beats Fit Pro, link logging on. Expect `AAC rate 128000 -> 96000 (gaps)` shifts tracking `rssi`/`lq` dips, `stall N ms -> re-conn` replacing the multi-second freezes, and audibly fewer/shorter dropouts. Also confirm the `... N dropped ...` ring-overflow lines don't swallow the new signals (bump the ring if they do). |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  apps/debug_menu.c → dbg_bt_a2dp()                          │
│   ↓                                                          │
│  bt-a2dp.c           BT lifecycle: scan, pick, persist      │
│   ├── inquiry / picker UI                                    │
│   └── A2DP signaling event handler                           │
│        ↓                                                     │
│  bt-pcm-sink.c       pcm_sink_t impl + SBC encoder          │
│   ├── sink_play(addr, size)  ← Rockbox upper layer          │
│   ├── pull_pcm()             ← drives complete_callback     │
│   └── audio_tick (BTstack timer)                            │
│        ↓                                                     │
│  BTstack (vendored)  HCI / L2CAP / SDP / AVDTP / A2DP       │
│   ↓                                                          │
│  bt-btstack-hal.c    btstack_uart_t bridge + hal_time_ms    │
│   ↓                                                          │
│  uart-x1000.c        Interrupt-driven UART, ring buffer     │
│   ↓                                                          │
│  bt-erosqnative.c    Power sequence, patchram upload        │
│   ↓                                                          │
│  X1000 hardware      UART0 @ PC10-13 func0, BCM4343A1       │
└─────────────────────────────────────────────────────────────┘

Music data path:
  codec → DSP → mixer → pcm_sw_volume (24-bit) → pcm_sink (BT) → SBC or AAC-LC(LATM) → BTstack → BCM4343A1 → speaker
```

## Hardware (HW4)

| Pin | Function | Direction | Purpose |
|-----|----------|-----------|---------|
| **PB26** | CLK32K (func0) | output | **32.768 kHz LPO** to BCM chip. ROM will not run without it. |
| PC10 | UART0_RXD (func0) | input | Host RX ← chip TX |
| PC11 | UART0_TXD (func0) | output | Host TX → chip RX |
| PC12 | UART0_CTS (func0) | input | Host CTS ← chip RTS (flow control) |
| PC13 | UART0_RTS (func0) | output | Host RTS → chip CTS (flow control) |
| PC18 | BT_REG_ON | output | BT subsystem enable |
| PC19 | HOST_WAKE_BT | output | Host → chip wake signal |
| PC20 | BT_WAKE_HOST | input | Chip → host wake |
| PC21 | WL_REG_ON | output | WiFi subsystem enable (not used for BT but the BCM combo chip requires it before BT_REG_ON) |

Pins defined in `gpio-target.h` but **not** actively used by current code: `BT_PWR = PC22`. On HW4, the chip works without driving PC22; module main power is supplied by the regulator rail rather than a host GPIO.

### Power-on sequence (final)

The BCM chip needs a 32.768 kHz reference before its internal ROM will run. Once that clock is supplied, the rest of the sequence follows the stock kernel pattern.

```
1. PB26 ← CLK32K (func0)               // 32 kHz LPO running
2. wait 10 ms
3. PC18, PC19, PC21 ← OUTPUT(0)        // hold chip in reset
   PC10-13          ← INPUT             // release UART so we don't fight the chip
4. wait 200 ms
5. PC21 ← HIGH                          // WL_REG_ON
6. wait 63 ms
7. PC18 ← HIGH                          // BT_REG_ON, ROM begins boot
8. wait 150 ms                          // ROM init
9. PC19 ← HIGH                          // HOST_WAKE_BT
10. wait 50 ms
11. PC10-13 ← UART0 func0
12. uart_x1000_init() @ 115200 8N1, MDCE|RTS hardware flow control
```

After step 12 the chip responds to `HCI_RESET`. We then upload the BCM4343A1 patchram (see below), wait 500 ms for the chip's automatic reboot, issue `HCI_RESET` again, and switch UART to 3 Mbps via vendor command 0xFC18.

`gpio_force_high()` in `bt-erosqnative.c` is used instead of plain `gpio_set_level()` because the X1000 GPIO controller's `gpio_set_level` only writes PAT0 — it has no effect on a pin that isn't already configured for output via PAT1/MASK/INT, and several of these pins arrive in the wrong mode from the bootloader. The function uses the X1000's GPIO Z hold-and-load registers to atomically write all four pin-state registers at once.

### Patchram upload

Without patchram, the chip's ROM only knows the read-version HCI commands; it can't do inquiry, pairing, or anything useful. The patchram is a binary blob (`BCM4343A1_001.002.009.0122.0538.hcd`) extracted from the stock Linux firmware and stored at `/.rockbox/BCM4343A1.hcd`. Upload protocol:

1. Send `HCI_VENDOR_DOWNLOAD_MINIDRIVER` (0xFC2E)
2. Stream HCI command records from the .hcd file, one at a time, each followed by Command Complete
3. Wait 500 ms for chip auto-reboot
4. `HCI_RESET` to confirm the new firmware is running

Implemented in `bt-bcm-patchram.c`.

## Software components

### BTstack (vendored)

Vendored under `firmware/drivers/btstack/` from BTstack master (the `version.h` says 1.6.2 but the headers are post-1.6.2; the master tip was needed for the A2DP source API we use). Files included:

- `src/hci.c`, `hci_cmd.c`, `gap.c`, `btstack_event.c`, `btstack_memory.c`, `btstack_run_loop_base.c`, `btstack_util.c`
- `src/l2cap.c`, `l2cap_signaling.c`
- `src/classic/`: `a2dp.c`, `a2dp_source.c`, `avdtp.c`, `avdtp_initiator.c`, `avdtp_acceptor.c`, `avdtp_source.c`, `avdtp_util.c`, `sdp_client.c`, `sdp_server.c`, `btstack_link_key_db_memory.c`, `btstack_sbc_bluedroid.c`
- `platform/embedded/`: `btstack_run_loop_embedded.c`, `btstack_uart_block_embedded.c`, `hci_transport_h4.c`
- `3rd-party/bluedroid/encoder/srce/`: SBC encoder source files
- `3rd-party/bluedroid/decoder/include/`: decoder headers (only — needed because `btstack_sbc_bluedroid.h` includes them; actual decoder .c files are not vendored)

### HAL bridge — `bt-btstack-hal.c`

BTstack's embedded platform expects three interfaces from us:

- `hal_time_ms()` returning milliseconds since boot. Implemented as `current_tick * (1000 / HZ)` (HZ=100 → 10 ms resolution). The 10 ms granularity is why `bt-pcm-sink.c` uses `AUDIO_TIMEOUT_MS=1` (see Audio pacing below).
- `btstack_uart_t` block-mode interface (init, set_baudrate, set_flowcontrol, send_block, receive_block, set_block_received, set_block_sent). The HAL implements these on top of `uart-x1000.c`.
- `hal_cpu_enable_irqs_and_sleep()` — sleeps the BT thread on a wakeup semaphore until either the next BTstack timer is due (computed from `btstack_run_loop_base_get_time_until_timeout`) or an event signals the semaphore. Without a real sleep here the run loop spins and starves the codec/UI threads.

Notification model: when the UART RX ISR fills the ring it both calls BTstack's per-block callback (so `hci_transport_h4` consumes complete packets) and releases the BT-thread wakeup semaphore (`bt_btstack_hal_signal()`), which wakes the run loop immediately. The HAL does **not** push bytes into a BTstack callback synchronously from ISR context — that pattern was tried earlier and caused parser desync when partial consumption happened.

### BT service — `bt-service.{c,h}` (and the BT thread)

The persistent BT background service. Owns the BTstack run loop (in a dedicated kernel thread `bt_service`, stack ~8.5 KB, priority `PRIORITY_PLAYBACK_MAX` = 5, higher than codec's 16), the entire BT state machine, all BTstack interaction, and all packet handlers. Exposes a control + status API (`bt_service_enable/disable/scan_start/connect_last/connect_addr/disconnect/get_state/get_status_msg/get_connected_name/get_scan_results/have_last/get_last`) that any UI screen can call. UI screens are pure consumers — they never touch BTstack directly.

Threading contract:
- All BTstack calls happen on the BT thread.
- Commands from foreground are posted as a single pending command word + bd_addr argument, picked up by the BT thread once per run-loop iteration. `bt_btstack_hal_signal()` wakes the thread immediately.
- Status reads are lock-free (single BT-thread writer, multi-thread readers; word reads are atomic on MIPS).
- `bt_service_get_scan_results` snapshots under `disable_irq_save` to avoid tearing during a concurrent inquiry-result write.

Why the dedicated thread matters: without it, the BTstack run loop ran on the foreground UI thread and yielded to other Rockbox threads between iterations. `execute_once` averaged ~52 ms per iteration. Since the canonical A2DP source pattern sends one packet per timer tick and the timer can only fire once per `execute_once`, packet rate was capped at ~19 packets/s — far below the ~70+ needed for 44.1 kHz SBC at typical max-payload sizes. The audio sounded like half-second spurts every half-second.

### Phase B UI — `apps/menus/bluetooth_menu.c`

Single-screen UI on top of the service. Reachable two ways:

- **Front page**: root menu entry `Bluetooth` (added in `apps/root_menu.{c,h}` as `GO_TO_BLUETOOTH` + `btscrn` handler + `bluetooth_root_item` MENUITEM_RETURNVALUE entry).
- **Settings**: General Settings → Bluetooth (Settings menu wraps the same `bt_open_screen()`).

Renders into the full content viewport (`viewport_set_fullscreen`) below the status bar, with `vp.font` forced to `screen->getuifont()` so it matches the regular menu font instead of inheriting whatever the SBS theme declared for its info viewport. Polls service status every 250 ms. Buttons are context-sensitive (`PLAY` = enable / connect / disconnect depending on state, `MENU` = scan / rescan / retry, scroll wheel navigates scan results, `BACK` leaves the screen but **the BT service keeps running** so audio continues while the user navigates the rest of Rockbox).

The legacy `Debug → Bluetooth A2DP` (in `firmware/.../bt-a2dp.c`) is still wired up — it's now a thin viewer on the same service API, useful as a development debugging screen.

### Cross-thread PCM safety — mutex in `bt-pcm-sink.c`

The BT sink runs `pull_pcm` (which calls `pcm_play_dma_complete_callback` + `pcm_play_dma_status_callback(STARTED)`) from the BT thread. The playback thread independently mutates `pcm_sw_volume.c`'s globals (`src_buf_addr`, `src_buf_rem`, `pcm_dbl_buf_num`) inside `pcm_play_data → start_pcm` on track changes. Without serialization the BT thread sees torn state and crashes in `pcm_scale_buffer_cut` reading from a wild source pointer (typically `0x400` = `NULL + pcm_dbl_buf_size[N]/X`).

Fix: `bt_pcm_sink` keeps a `struct mutex s_pcm_lock`. `sink_lock`/`sink_unlock` (called by upper-layer `pcm_play_lock`) acquire/release it; `pull_pcm` brackets the `complete_callback` + `STARTED` pair with the same mutex. Recursive (`mutex_lock` increments `m->recursion` if the same thread already holds it), so the nested `sink_play` call inside `start_pcm` works without special-casing. Additionally, `pull_pcm` checks `pcm_is_playing()` before calling `complete_callback` — `pcm_play_dma_stop_int` sets `src_buf_addr = NULL` but leaves `pcm_dbl_buf_size[]` non-zero, so a still-firing `audio_tick` would otherwise still get a stale "true" from `complete_callback` and compute `addr = NULL + ~1024 = 0x400`.

### Volume routing — `audiohw-erosqnative.c`

On the ES9018K2M-equipped F20, `audiohw_set_volume` normally sends gain over I2C to the DAC and pins the software-volume to ~0 dB. When the BT sink is the active output the DAC is no longer in the path, so I2C gain does nothing audible. The `bt_pcm_sink_is_active()` check in `audiohw_set_volume` reroutes the user's volume to `pcm_set_master_volume(l, r)` — i.e. through the software scaling in `pcm_sw_volume.c`. When BT goes inactive, normal DAC routing resumes.

### UART driver — `uart-x1000.c`

Generic, interrupt-driven UART for any X1000 UART controller. Operates as: ISR → ring buffer → consumer pulls on demand via `uart_x1000_rx_read()`. Hardware flow control (MDCE+RTS) is enabled.

For high baud rates (3 Mbps), the UART's UMR (oversample) divider is reduced from the default 16x to 8x to keep the divisor in range with the 24 MHz EXCLK source. See `uart-x1000.c` for the adaptation logic.

### Baud-rate switch (115200 → 3 Mbps)

After patchram upload + the second `HCI_RESET`, `bt_thread_main` issues HCI vendor command `0xFC18` (Update_Baudrate, BCM-specific) with payload `00 00 + LE32(3000000)`. After the chip ACKs at the old baud, its UART firmware reconfigures internally — the receiver needs ~50 ms to retrain. We then `mdelay(50)` → `hal_uart_dma_set_baud(3000000)` → `mdelay(50)` → drain RX (discard transition garbage) → probe with `HCI_Read_Local_Version` to confirm sync. `hci_transport_config_uart_t.baudrate_init` is then set to `3000000` so BTstack's H4 transport doesn't drop us back to 115200 when it opens.

Without this switch, every ~670-byte A2DP media packet takes ~60 ms to transmit and the effective send rate caps at ~19 packets/s — far below the ~70+ needed for 44.1 kHz SBC. With the switch, host UART throughput is 3 Mbps and packet rate is bounded only by the audio_tick rate.

The `s_current_baud` static in `bt-btstack-hal.c` tracks the post-switch rate so a subsequent `hal_uart_dma_init` re-init doesn't clobber the divisor back to the init constant.

### A2DP source profile — `bt-a2dp.c`

State machine: `INIT → CONFIRM_LAST → SCAN → PICK → PLAY → DONE`. Owns the BT lifecycle but no audio-pacing logic.

Key configuration:

- `gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT)` — Just Works pairing with auto-accept.
- `gap_ssp_set_auto_accept(1)` plus an explicit `gap_ssp_confirmation_response()` in the `USER_CONFIRMATION_REQUEST` handler.
- `hci_set_master_slave_policy(0)` — don't request master role (some devices refuse role switch).
- `hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR)` — get RSSI and device names from inquiry.
- `gap_secure_connections_enable(false)` — older BR/EDR devices time out during SC negotiation (LMP Response Timeout, status 0x24). We can re-enable for modern peers later.
- `gap_set_security_level(LEVEL_2)` — Bose's A2DP PSM refuses unencrypted channels with `L2CAP_CONNECTION_RESPONSE_RESULT_REFUSED_SECURITY` (0x66).
- SBC capabilities advertised: 44.1 kHz, stereo, all block/subband modes, max bitpool 35 (dropped from 53 in C10b for Apple H1 sinks).
- AAC endpoint (registered first): MPEG-4 AAC LC, 44.1 kHz only, stereo, up to ~320 kbps. Codec choice is explicit (`ENABLE_A2DP_EXPLICIT_CONFIG`) in bt-service: prefer AAC when the sink's caps actually cover 44.1/stereo/MPEG-4-LC, else SBC. See gotchas 31–32.
- Local SDP record advertises A2DP source role.

A critical lesson: do **not** split bonding from stream establishment. We tried `gap_dedicated_bonding` first, then waited for post-bond disconnect, then `a2dp_source_establish_stream`. That pattern reliably produces L2CAP error 0x66 because the second ACL doesn't re-establish encryption cleanly before AVDTP opens. Calling `a2dp_source_establish_stream` directly on `HCI_STATE_WORKING` lets BTstack do pair + encrypt + AVDTP atomically, matching what BTstack's own `a2dp_source_demo` does.

### Persistent device — `bt_last.dat`

After a successful `STREAM_ESTABLISHED` (status 0), `bt-a2dp.c` writes the connected device's address and EIR name to `/.rockbox/bt_last.dat` (30-byte struct, magic `BTL1`). On the next run, the user is prompted **"Reconnect last? PLAY=connect MENU=rescan BACK=cancel"** before scanning kicks off. Saving requires a stream to actually open, so a partial connect never overwrites a previously-good record.

### Persistent link keys — `bt_keys.dat` (C7)

`bt-tlv.c` implements `btstack_tlv_t` against a fixed-size file at `/.rockbox/bt_keys.dat`. BTstack's `btstack_link_key_db_tlv` adapter (vendored from upstream) is wired to it via `hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(&bt_tlv_impl, NULL))`. With this in place, link keys survive reboots and the `Reconnect last?` flow no longer fails with HCI disconnect reason `0x06` ("PIN or Key Missing"). `NVM_NUM_LINK_KEYS = 8` slots; the file is rewritten atomically (tmpfile + rename) on every store/delete.

### PCM sink — `bt-pcm-sink.c`

Implements `pcm_sink_t` so it can be installed as the active output via `pcm_set_current_sink(PCM_SINK_BT)`. When BT is the active sink, Rockbox's playback engine submits PCM buffers to it the same way it would submit to the X1000 DAC.

```
Rockbox playback → mixer → pcm_sw_volume (24-bit volume scaling)
                            ↓ sink.play(addr, size)
                          BT sink stores buffer ptr
                            ↓ (BTstack run-loop timer, every 10 ms)
                          pull_pcm(): consumes samples at SBC rate
                            ↓ when buffer empty:
                          pcm_play_dma_complete_callback() → next buffer
                          pcm_play_dma_status_callback(STARTED) → triggers refill
                            ↓
                          SBC encode → media-packet assembly
                            ↓ on can_send_now:
                          a2dp_source_stream_send_media_payload_rtp()
```

Sample format on F20: `PCM_NATIVE_BITDEPTH=24`, so each sample is a signed 24-bit value LSB-aligned in an `int32_t` (top 8 bits sign-extended). 8 bytes per stereo frame, not 4. `pull_pcm()` shifts each sample right by 8 to recover an `int16_t` for the SBC encoder. With `HAVE_SW_VOLUME_CONTROL` defined for this target, the buffer the sink receives is the volume-scaled `pcm_dbl_buf`, not the raw 16-bit mixer output.

**Audio pacing.** The canonical BTstack A2DP demo uses `AUDIO_TIMEOUT_MS=10` and relies on the timer firing at ~100 Hz to send one packet per tick. On targets with finer `hal_time_ms` resolution that just works. Ours has 10 ms granularity (`current_tick * 10`), so a 10 ms timer re-arms to the *next* boundary, giving only ~50 Hz — which at typical ~5 SBC frames/packet × 128 samples = ~32 k samples/s, below the 44.1 k we need. With `AUDIO_TIMEOUT_MS=1` the timer always re-arms in the immediate next 10 ms tick boundary, so we fire at the full ~100 Hz, giving ~64 k samples/s — comfortably above 44.1 k.

Sample rate: the sink advertises only 44.1 kHz in its caps. Rockbox should resample non-44.1 tracks via DSP before they reach the sink. (Verifying this is part of C6a follow-up.)

### Sink-swap plumbing — `pcm.c`

`firmware/pcm.c` was extended from a single hard-coded sink to an indexed array selectable via `pcm_set_current_sink(enum pcm_sink_ids)`. Swap protocol:

```c
void pcm_set_current_sink(enum pcm_sink_ids new_sink) {
    if (new_sink == cur_sink) return;
    enum pcm_sink_ids old_sink = cur_sink;
    sinks[old_sink]->ops.lock();
    if (pcm_playing) sinks[old_sink]->ops.stop();
    sinks[old_sink]->ops.unlock();

    /* Capture upper-layer callbacks; clear pcm_playing so the next
     * pcm_play_data() takes the standard "fresh start" path. */
    pcm_play_callback_type   saved_get_more = pcm_callback_for_more;
    pcm_status_callback_type saved_status   = pcm_play_status_callback;
    bool was_playing = pcm_playing;
    pcm_playing = false;
    cur_sink = new_sink;

    if (was_playing && saved_get_more)
        pcm_play_data(saved_get_more, saved_status, NULL, 0);
}
```

The crucial detail is going through `pcm_play_data()` rather than calling `sink.play()` directly. `pcm_play_data` routes through `pcm_sw_volume.c`'s `pcm_play_dma_start_int` override, which fires `STARTED` twice to dual-fill both `dbl_buf` slots before invoking `sink.play(dbl_buf[1], ...)`. Bypassing that path leaves one `dbl_buf` slot empty and yields alternating audio/silence frames at the dbl_buf cadence.

`pcm_sink_ids` is conditionally extended with `PCM_SINK_BT` via `HAVE_BT_PCM_SINK`, defined in `firmware/export/config/erosqnative.h` (skipped in BOOTLOADER builds).

### Jack-detect override — `button-erosqnative.c`

Rockbox's playback engine pauses on `SYS_PHONE_UNPLUGGED` per the `unplug_mode` setting. On F20 with no headphones plugged in, `headphones_inserted()` polled by `firmware/drivers/button.c` returns false, the unplug event posts, and `audio_pause()` halts the codec thread — track time stops advancing. Without an override, BT can't deliver music if no headphones are plugged in.

The fix: while `bt_pcm_sink_is_active()` is true, `headphones_inserted()` returns true unconditionally. `button.c`'s normal poll picks up the transition, debounces, and posts `SYS_PHONE_PLUGGED`, which calls `unpause_action()` and resumes the codec thread. On exit (BT sink stops), the next poll sees the real jack state again, and if no headphones are present, `SYS_PHONE_UNPLUGGED` fires through the normal channel.

This route reuses Rockbox's existing pause/unpause plumbing instead of creating a parallel "BT-active" state machine.

## Critical gotchas

These are the non-obvious facts that, if missed, lose hours.

1. **PB26 CLK32K is mandatory**, not optional. The chip's ROM does not boot without a 32.768 kHz LPO reference. Earlier hypotheses (e.g. PC22 module main power) were wrong; PC22 isn't actively driven by current code.

2. **F20 PCM is 24-bit in 32-bit containers**. `PCM_NATIVE_BITDEPTH=24` means 8 bytes per stereo frame. Reading the buffer as `int16_t` pairs interprets each `int32_t` sample as two `int16_t` and produces garbage that sounds like harsh noise.

3. **`HAVE_SW_VOLUME_CONTROL` changes the buffer the sink sees**. With this define, `pcm_sw_volume.c` overrides `pcm_play_dma_complete_callback` and returns its own `pcm_dbl_buf` (volume-scaled, native bit depth). The sink does not see the raw 16-bit mixer output.

4. **Every buffer transition must fire `pcm_play_dma_status_callback(PCM_DMAST_STARTED)`**. That callback is what tells `pcm_sw_volume.c` to refill the *inactive* `dbl_buf` slot. Without it, `complete_callback` keeps returning the same slice indefinitely, producing a periodic looped tone. The X1000 sink fires it inside its DMA-complete handler; the BT sink must fire it inside `pull_pcm` after each successful `complete_callback`.

5. **Sink swap must use `pcm_play_data()`, not direct `sink.play(addr, size)`**. `pcm_play_data` is the standard entry that triggers `pcm_sw_volume`'s `start_pcm()`, which fires `STARTED` *twice* to dual-fill both `dbl_buf` slots before kicking off playback on the new sink. Calling `sink.play()` directly leaves one slot empty.

6. **HCI inquiry results may arrive multiple times per device** before all fields populate. The first result for a device often lacks an EIR name; subsequent results in the same scan often add it. The picker dedupes by address and merges fields (name, RSSI) as they arrive.

7. **Older BR/EDR speakers fail Secure Connections negotiation**. SC times out (LMP Response Timeout, status 0x24) on devices that only do legacy SSP. Disable with `gap_secure_connections_enable(false)`.

8. **Don't split bonding and stream establishment**. Calling `gap_dedicated_bonding` first, waiting for post-bond disconnect, then `a2dp_source_establish_stream` causes L2CAP to refuse AVDTP with `L2CAP_CONNECTION_RESPONSE_RESULT_REFUSED_SECURITY` (0x66). Call `a2dp_source_establish_stream` directly on `HCI_STATE_WORKING`; BTstack handles pair + encrypt + AVDTP atomically.

9. **`gpio_set_level()` on the X1000 is not atomic**. It only writes PAT0 and has no effect if INT/MASK/PAT1 aren't already configured for output. Use the GPIO Z hold-and-load mechanism (`gpio_force_high()` in `bt-erosqnative.c`) for any pin whose initial state from the bootloader is unknown.

10. **The UART RX ISR must not auto-drain the ring into a consumer callback**. An earlier design passed `(data, len)` to the consumer and advanced the ring tail unconditionally; if the consumer only consumed part of `len`, the rest was silently dropped, causing H4 parser desync (manifested as bogus event "0x04 sz=5 pl=3"). The current design pushes to the ring + emits a notification; the consumer pops on demand via `uart_x1000_rx_read()`.

11. **The BTstack run loop must not be driven from the foreground UI thread.** When it was, `execute_once` averaged ~52 ms per iteration because the foreground yielded to other Rockbox threads between iterations. The canonical A2DP source pattern fires one timer per `execute_once` and sends one packet per tick, so packet rate gets capped at ~19/s — far below the ~70+ needed for 44.1 kHz SBC. Fix: spawn a dedicated kernel thread at `PRIORITY_PLAYBACK_MAX` that owns the run loop; have `hal_cpu_enable_irqs_and_sleep` sleep on a wakeup semaphore that the UART RX ISR signals. Iterations then happen at the natural rate (next timer due / next ISR), and 1-packet-per-tick keeps up.

12. **`hci_transport_config_uart_t.baudrate_init` is the post-open baud — not the pre-init baud.** When the H4 transport opens, it calls `hal_uart_dma_set_baud(baudrate_init)`. If you've already manually switched both sides to 3 Mbps before calling `hci_init`, leaving `baudrate_init = 115200` will silently drop the host UART back down while the chip stays at 3 Mbps, killing comms. Match this field to whatever rate the link is actually on by the time BTstack opens the transport.

13. **`hal_time_ms` granularity affects the effective `AUDIO_TIMEOUT_MS`.** With `current_tick * 10` (10 ms steps), `set_timer(t, 10)` re-arms to the *next* boundary, giving ~50 Hz, not the canonical 100 Hz. Use `AUDIO_TIMEOUT_MS=1` to always land in the immediate next tick boundary and fire at the full 100 Hz.

14. **BCM4343A1 baud-switch (`0xFC18`) needs ~50 ms settle on each side.** The chip's firmware reconfigures its UART *after* sending the Command Complete and the receiver needs ~50 ms to retrain. `mdelay(10)` was insufficient — host TX into a not-yet-locked receiver gets dropped. Sequence: ACK → `mdelay(50)` → switch host divisor → `mdelay(50)` → drain RX (transition garbage) → optional probe via `HCI_Read_Local_Version` to confirm sync.

15. **`pcm_play_dma_stop_int` clears `src_buf_addr` to NULL but leaves `pcm_dbl_buf_size[]` non-zero.** A still-running BT `audio_tick` will then see `complete_callback` return true (because `pcm_dbl_buf_size[num] != 0`) and the next `STARTED` will compute `addr = NULL + ~1024 = 0x400`, crashing `pcm_scale_buffer_cut`. Gate the BT `pull_pcm`'s call to `complete_callback` on `pcm_is_playing()` so this can't happen between tracks.

16. **The BT thread and the playback thread both touch `pcm_sw_volume.c` globals** (`src_buf_addr`, `src_buf_rem`, `pcm_dbl_buf_num`). The X1000 sink is implicitly safe because its `complete_callback` runs in ISR context and is serialized by the IRQ-disable inside `pcm_play_lock`. The BT sink runs from a thread, so it needs a real mutex. `bt_pcm_sink` uses Rockbox's recursive `struct mutex` — `sink_lock`/`sink_unlock` acquire/release, `pull_pcm` brackets the `complete_callback` + `STARTED` pair with the same mutex.

17. **A2DP `establish_stream` returns `ERROR_CODE_COMMAND_DISALLOWED` (0x0C, decimal 12) when the remote auto-reconnects faster than we initiate.** The speaker, on power-up, immediately reconnects to a remembered Rockbox source. By the time the user presses "Connect Last", AVDTP already has an in-progress connection and refuses our redundant call. Per BTstack's source comment "the stream will get set-up nevertheless" — treat `rc=12` as a no-op success and let `SIGNALING_CONNECTION_ESTABLISHED` + `STREAM_ESTABLISHED` events drive the state to `STREAMING` on their own. Pick up the cid from the SIGNALING event so subsequent media sends have it.

18. **A user-screen taking over the LCD must use `viewport_set_fullscreen` + `screen->set_viewport(&vp)`, not bare `lcd_clear_display`/`lcd_puts`.** The Settings menu leaves the SBS theme's info viewport active when calling into a function item, so bare `lcd_*` calls only clear/draw inside that viewport (often narrow or wrong-font). Force the UI font with `vp.font = screen->getuifont()` so the screen text matches regular menu lists rather than the SBS info area's font.

19. **`struct viewport vp;` must be zeroed before `viewport_set_fullscreen` / `viewport_set_defaults`.** Both call `init_viewport(vp)` which tests `vp->buffer != NULL` and then dereferences `vp->buffer->elems`. Stack-resident `vp.buffer` is uninitialized garbage; if it's non-NULL non-pointer, the deref produces a tiny "framebuffer" that `lcd_clear_viewport` then hands to `memset16` with a NULL-ish destination — TLB refill in `memset16+0x8` (BadVAddr=0x2). The Settings → Bluetooth path happens to leave NULL there so it works; the root-menu path leaves a small non-NULL value and crashes deterministically. Always `memset(&vp, 0, sizeof(vp))` first.

20. **Boot autoconnect must arm a flag in bt-service rather than block in `main()` waiting for HCI WORKING.** HCI bring-up takes ~3-5 s (patchram upload, UART switch, stack init). Blocking boot for that delays the file browser. The fix: `bt_service_enable_and_connect_last()` sets `s_autoconnect_on_ready = true` and returns immediately; the BT thread's `BTSTACK_EVENT_STATE → HCI_STATE_WORKING` handler consumes the flag and issues `do_connect(&s_bonded[0])`. `bonded_load()` must run BEFORE `hci_power_control(HCI_POWER_ON)` so the bonded array is populated by the time the WORKING event fires.

21. **EROS Q `ACTION_STD_CONTEXT` is bound to `BUTTON_MENU|BUTTON_REPEAT` (long-press MENU), not long-press PLAY.** Long-press PLAY fires `ACTION_STD_HOTKEY`, which is the user-configurable global hotkey — don't intercept it. Use `ACTION_STD_CONTEXT` (long MENU) for in-screen "secondary action on highlighted item" and document it explicitly in any hint text.

22. **Rockbox runtime image lives at `/.rockbox/rockbox.erosq` on the device, NOT the top-level `rockbox.erosq`.** The top-level file is the bootloader-update image (only consumed during a firmware-update flow). Copying a build to the top level looks like a successful deploy but the device keeps running the stale `.rockbox/rockbox.erosq`. Symptom: exception PCs don't shift between builds even after code changes that should move them. Always deploy with `cp build-erosqnative/rockbox.erosq /run/media/avery/F20/.rockbox/rockbox.erosq && sync`.

23. **Apple H1-class sinks (AirPods, Beats Fit Pro, etc.) are picky about SBC bitpool and sniff mode.** They'd prefer AAC (which we don't offer) and treat SBC as a fallback path. Default A2DP source max_bitpool of 53 causes periodic glitches/blips on these sinks specifically; non-Apple sinks (Bose, Redmi) tolerate 53 fine. Cap to ~35 in `sbc_caps[3]`. Separately, Apple buds aggressively initiate sniff mode for battery, and active↔sniff transitions stall media. Set `gap_set_default_link_policy_settings(LM_LINK_POLICY_DISABLE_ALL_LM_MODES)` to refuse peer sniff requests for the lifetime of the stream.

24. **BR-only ACL packet types beat EDR for body-blocking robustness on H1 buds, with no audio-quality cost.** `hci_enable_acl_packet_types(ACL_PACKET_TYPES_BR)` disables 2-DH*/3-DH*. EDR needs ~5-9 dB more SNR than 1 Mbps GFSK BR; in a body-shadow null (source on torso, primary bud across the head) EDR collapses into a retransmit cascade audible as a long tear, while BR loses individual frames cleanly. SBC at bitpool 35 is ~250 kbps payload — fits in 1-DH5's ~700 kbps usable, so no quality is lost.

25. **HCI automatic flush timeout (`gap_enable_link_watchdog`) breaks Beats Fit Pro connections.** Both 30 ms (2026-04-29 attempt) and 50 ms (2026-05-03 attempt) caused the Beats to disconnect ~5 s after every successful connect; non-Apple sinks were unaffected. Either the bud's stack misinterprets flushed-packet boundary flags or it's deliberately rejecting links that mark media as flushable. Don't enable this knob for Apple H1 sinks — leave A2DP packets non-flushable (BTstack default) and rely on BR-only (gotcha 24) for retransmit-cascade resilience.

26. **`avrcp_connect()` allocates two connection slots, not one.** It registers both CONTROLLER and TARGET roles for the same peer. With `MAX_NR_AVRCP_CONNECTIONS = 1` (BTstack default in our `btstack_config.h`) the second alloc fails and the call returns `0x56` (`BTSTACK_MEMORY_ALLOC_FAILED`) — silent unless you log the rc. Set `MAX_NR_AVRCP_CONNECTIONS = 2`.

27. **Beats Fit Pro (and likely other H1 buds) won't open AVCTP from the sink side.** AVRCP target alone with no source-side `avrcp_connect()` produces a connected, streaming A2DP session with zero AVRCP traffic — bud taps do nothing because the bud is a controller and never sees a controller-capable peer to talk to. Fix: call `avrcp_connect(addr, &cid)` from the source on `A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED`. Bose may have been initiating it for us; BFP doesn't.

28. **Without playback-status notifications, BFP only ever sends AVRCP PAUSE (0x46) on its play/pause button**, never PLAY (0x44). Its internal model assumes the source is in the "playing" state and never updates without `NOTIFICATION_PLAYBACK_STATUS_CHANGED` from us. A literal `PAUSE → audio_pause` map only works for the first tap; subsequent taps are dropped because we're already paused. Cheap fix: treat both PLAY and PAUSE as a toggle against `audio_status() & AUDIO_STATUS_PLAY/PAUSE`. Proper fix would be to send playback-status notifications (C11 v2 (a)).

29. **A2DP AAC to Apple H1 sinks must be LATM, not raw access units.** Raw AUs (correct size, correct content) decode to *total silence* on Beats Fit Pro. Wrap each AU in a LATM `AudioMuxElement` with `muxConfigPresent=1` (StreamMuxConfig + ASC in every packet — stateless), matching AOSP's `TT_MP4_LATM_MCP1`, which is what every Android phone sends. The A2DP spec text says "LATM shall not be used"; real sinks are tested against phones, not the spec. Hand-rolled muxer in `bt-aac-encoder-voaac.c` (vo-aacenc only emits raw/ADTS).

30. **The SBC media payload header's frame count is a 4-bit field.** Bits 4–7 are fragmentation flags. Packing more than 15 frames into one packet (easy at low bitpool — ~44 B/frame at bitpool 16) spills the count into the flag bits, corrupting the header; the sink drops or garbles every packet, audible as total silence. At bitpool 35 (~83 B frames) a ~672 B payload never reaches 15 frames, so the bug hides until you lower the bitpool. Cap at `SBC_MAX_FRAMES_PER_PACKET 15`.

31. **`rc==0` from `a2dp_source_set_config_*` means the request was QUEUED, not accepted.** The sink's answer arrives later as a MEDIA_CODEC configuration event (accepted), as `A2DP_SUBEVENT_COMMAND_REJECTED` (refused), or **never** (the Redmi ignores AAC configs it doesn't like). Handle the reject event AND run a watchdog (4 s) on any in-flight AAC config. A same-connection retry is impossible: after a reject the a2dp config state is `A2DP_CONNECTED` and `a2dp_config_process_config_init` only accepts `A2DP_DISCOVERY_DONE`, so the SBC fallback must disconnect and reconnect (one-shot `s_force_sbc` flag through the pending-switch machinery). Symptom when unhandled: state stuck CONNECTING with the ACL alive, Rockbox playing into the DAC, speaker "connected" but silent — log signature `sel AAC rc=0` followed by nothing.

32. **Intersect the sink's AAC capability bitmaps before configuring.** Object type, sampling-frequency and channel bitmaps in the caps event are the sink telling you what it can do — configuring outside them (we hardcoded 44.1/stereo/MPEG-4-LC) gets a reject or worse, silence. Note btstack shifts the raw AVDTP bytes in the event: object_type bit 5 = MPEG-4 LC, sampling bit 4 = 44.1 kHz, channels bit 2 = stereo. Log the raw caps (`AAC caps ot=.. sf=.. ch=..`) so the next odd sink is diagnosable from disk.

33. **`bt_pcm_sink_stop_streaming` frees the AAC encoder — and runs on STREAM_SUSPENDED, not just RELEASED.** On resume, STREAM_STARTED fires but the MEDIA_CODEC configuration event does not (config events only fire during negotiation), so nothing re-created the encoder: `audio_tick` bailed on the NULL encoder forever and the stream resumed permanently silent while the UI said STREAMING. `start_streaming` now rebuilds it from the saved `s_aac_cfg`. SBC never hit this only because its encoder state isn't torn down on stop.

34. **AVDTP refuses payloads larger than the remote media MTU and LATM has no fragmentation.** `avdtp_source_stream_send_media_payload_rtp` returns `ERROR_CODE_MEMORY_CAPACITY_EXCEEDED` (0x07) and the frame is simply gone. vo-aacenc's "CBR" spikes well above nominal (bit reservoir — ~490 B seen at 128 kbps, ~380 B average), so a small-MTU sink loses frames sporadically, or everything if the MTU is below every frame. The sink checks `s_max_payload` before sending, drops with a distinct `AAC drop` log line, and advances the RTP timestamp so the sink's media clock stays consistent.

35. **Never advertise a PCM rate the audio path can't deliver.** On an incoming connection (every speaker auto-reconnect) the sink is the AVDTP initiator and may configure our endpoint at any advertised rate. We advertised 44.1+48 kHz AAC while `bt_samprs` is `{44100}` — a 48 kHz pick would label 44.1 samples as 48 kHz: ~9% pitch shift plus a consume-rate mismatch. AAC caps now advertise 44.1 only.

36. **The negotiated AAC `bit_rate` is a *maximum*, so the encoder can be swapped to a lower rate mid-stream with zero renegotiation.** LATM AudioMuxElements are self-contained (no rate field anywhere), so the sink just decodes whatever arrives. This is what Apple sources do on marginal RF, and it's the basis of the C13 adaptive bitrate: rebuild vo-aacenc at an idle point between frames (audio_tick, payload empty). vo-aacenc re-primes over ~2 frames (~46 ms) after a rebuild — the sink's jitter buffer rides it out. Corollary: at 64 kbps the ~190 B average frame fits a single DH3 baseband packet instead of spanning three, so a downshift buys margin twice (less airtime *and* fewer fragments to lose).

37. **Link supervision will not rescue a frozen media path.** Supervision only needs *some* LMP traffic to get through; a retransmit-wedged ACL can freeze media near-indefinitely without disconnecting (21 s send gap observed with the link alive throughout). Detect it host-side (no successful media send for N s → reconnect) and tear down with `gap_disconnect`, **not** `a2dp_source_disconnect` — the AVDTP close handshake would ride the same wedged ACL, while an HCI disconnect completes locally even if the peer never answers the LMP detach.

38. **BR/EDR `Read_RSSI` is not absolute dBm by spec** — it's dB relative to the "golden receive range" (0 = inside it, negative = below), though BCM controllers commonly report something close to real dBm. `Read_Link_Quality` is vendor-scaled 0–255, higher = better (BCM derives it from CRC/retransmit rate). Treat both as trend data, not calibrated measurements; `lq` sagging while `tx` stays 12/12 = the link is out of margin with nothing left to give.

39. **`sscanf` is declared in Rockbox's libc headers but NOT linked into the core firmware build** (plugins only). Core code that calls it compiles fine and dies at link time (`undefined reference to sscanf`). Parse with `strtol` by hand — see `settime_check_file()` in `apps/misc.c`. Don't "clean up" that parser back to sscanf.

## Non-BT fork features (wheel + clock)

These ride the same fork but have nothing to do with Bluetooth. All are erosq-gated via config defines so they compile out with one line.

### Scroll-wheel acceleration + fast-scroll letter overlay (commits `902bb14d8c`, `d6d15d1905`, `bd842df267`, `725bfa8aa9`)

iPod-classic-style wheel feel. Stock Rockbox had NO wheel acceleration on this target (the "List Acceleration" settings only affect held buttons, never the wheel).

- **Driver** (`button-erosqnative.c`, behind `HAVE_WHEEL_ACCELERATION` in `erosqnative.h`): a decaying activity counter (charge 16/detent, drain 4/10 ms poll, engage knee at 32, cap 80) computes a list-step multiplier posted in button-data bits 24..30 (bit 31 clear — the e200v2 scheme; `button_apply_acceleration()` returns it as-is). Counter **hard-resets** after a 100 ms detent gap or direction reversal, so a click after any pause moves exactly 1 item. Knife-edge ≈ 25 detents/s: below it everything is 1:1.
- **Strength setting**: "Wheel Acceleration" (Off/Weak ×4/Moderate ×8/Strong ×16), Settings > General > Display > Scrolling, applied via `button_wheel_set_accel()` (live + at boot from `settings_apply`).
- **Letter overlay** (`apps/gui/list.c`, behind `HAVE_WHEEL_SCROLL_LETTER`): step multiplier ≥ 4 in a 40+-item list pops a black card with the selected item's first letter (pixel-doubled out of the framebuffer via `FBADDR` — the fragile bit if upstream refactors the fb API again), expiring 0.5 s after the last fast event via a kernel `timeout_register` that posts `BUTTON_REDRAW`. Painted INTO the frame by a hook in `list_draw` (bitmap/list.c) right before its `update_viewport()` — never paint after the push and never push a second rect; both were tried and both flash/tear on camera. Togglable: "Fast-scroll Letter Popup" setting.
- **Anti-coast**: the framedrop guard in `gui_synclist_do_button` (skip redraw when `button_queue_count() >= FRAMEDROP_TRIGGER`) now applies on wheel targets too. Without it, per-event redraws are slower than a fast spin's event rate, the queue backs up, and the cursor coasts after the wheel stops.
- Merge-conflict magnets for future rebases: the `ACTION_STD_PREV/NEXT` cases in `gui_synclist_do_button` (modified upstream lines) and the one-call hook in `list_draw`.

### RTC sync from a host-written file (commit `bbf2878cca`)

Sets the clock over USB with zero on-device interaction. Host writes `/.rockbox/settime.txt` containing one line `YYYY-MM-DD HH:MM:SS` (**local** time, years 2000–2099) while the player is mounted, then unplugs:

```sh
date '+%Y-%m-%d %H:%M:%S' > /run/media/avery/F20/.rockbox/settime.txt && sync
```

- **Why on-unplug:** while USB-mounted the host owns the raw block device — firmware can't read the FS. `settime_check_file()` (`apps/misc.c`, behind `HAVE_SETTIME_FILE` in `erosqnative.h`) runs from the `SYS_USB_CONNECTED` handler right after `gui_usb_screen_run()` returns (disk just remounted; catches every USB session regardless of screen), plus a boot fallback in native `init()` after `settings_apply(true)` for a file left behind by a player powered off while plugged.
- Applies via `set_day_of_week()` (mandatory — `valid_time()` rejects an unset weekday) + `set_time()`, splashes `Clock set: ...`, and **always deletes the file** (parse failure included). Parser is strtol-based (gotcha 39).
- Caveat: the boot-fallback path sets the clock to the *written* timestamp, stale by however long the player sat unbooted — inherent, no reference clock exists. For accuracy: write fresh, unplug promptly. The unplug path is accurate to seconds.

## File map

### New / heavily modified for BT

| Path | Purpose |
|------|---------|
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-erosqnative.{c,h}` | Power sequence, pin defs, polled UART send/recv |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-bcm-patchram.c` | BCM4343A1 .hcd patchram uploader |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-btstack-hal.{c,h}` | btstack_uart_t + hal_time_ms + hal_cpu_sleep (semaphore-based) bridge |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-diag.c` | Debug screens: BT Diagnostic, BT Inquiry, BT Pair |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-service.{c,h}` | Persistent BT service: thread + state machine + control/status API |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-a2dp.c` | Legacy `Debug → Bluetooth A2DP` viewer (now a thin client of bt-service) + 0xFC18 baud switch (also lives in bt-service.c now) |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-pcm-sink.{c,h}` | `pcm_sink_t` impl + SBC/AAC codec dispatch + audio pacing + mutex against pcm_play_data races |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-aac-encoder.h` | Codec-agnostic AAC encoder interface (`BT_AAC_BACKEND` selects backend) |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-aac-encoder-voaac.c` | vo-aacenc backend (fixed-point, static arena) + hand-rolled LATM(MCP1) muxer — the active backend |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-aac-encoder-faac.c` | FAAC backend (abandoned — float path needs libm; kept for reference) |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-aac-encoder-stub.c` | Stub backend when no real encoder is vendored (AAC endpoint not registered) |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-link-log.{c,h}` | 256-entry link-event ring buffer, debug screen + append-on-pause dump to `/.rockbox/bt_link.log` (persisted "Link logging" toggle) |
| `firmware/target/mips/ingenic_x1000/erosqnative/vendor-voaac.sh` | Vendors vo-aacenc under `3rd-party/voaac/` (tree not committed) |
| `firmware/target/mips/ingenic_x1000/erosqnative/bt-tlv.{c,h}` | File-backed `btstack_tlv_t` for persistent link keys |
| `apps/menus/bluetooth_menu.c` | User-facing Bluetooth screen (`bt_open_screen`) + Settings → Bluetooth menu (open + autoconnect toggle) + bonded picker + forget confirm |
| `apps/root_menu.{c,h}` | Front-page `Bluetooth` entry (`GO_TO_BLUETOOTH` + `btscrn` dispatches via `do_menu(&bluetooth_menu, ...)`) |
| `apps/main.c` | Boot hook: calls `bt_service_enable_and_connect_last()` when `bt_autoconnect` setting is true |
| `apps/settings.h`, `apps/settings_list.c` | `global_settings.bt_autoconnect` (OFFON_SETTING gated on `HAVE_BT_PCM_SINK`) |
| `apps/lang/english.lang` | `LANG_BT_AUTOCONNECT_ON_BOOT` phrase |
| `firmware/target/mips/ingenic_x1000/uart-x1000.{c,h}` | Interrupt-driven UART driver, hardware flow control, high-baud UMR |
| `firmware/target/mips/ingenic_x1000/lcd-x1000.c` | Pre-existing — small fix for LCD crash on BT init |
| `firmware/target/mips/ingenic_x1000/erosqnative/button-erosqnative.c` | Added `bt_pcm_sink_is_active()` shortcut in `headphones_inserted()` |
| `firmware/pcm.c` | `sinks[]` indexed array, `pcm_set_current_sink()` |
| `firmware/export/pcm_sink.h` | `PCM_SINK_BT`, `PCM_SINK_COUNT`, `bt_pcm_sink` extern |
| `firmware/export/pcm.h` | `pcm_set_current_sink()` prototype |
| `firmware/export/config/erosqnative.h` | `HAVE_BT_PCM_SINK` define |
| `firmware/firmware.make` | BTstack include paths |
| `firmware/SOURCES` | New BT and BTstack source files |
| `firmware/drivers/btstack/...` | Vendored BTstack |
| `apps/debug_menu.c` | Wired BT Diagnostic / Inquiry / Pair / A2DP entries |

### Firmware blob

| Path | Purpose |
|------|---------|
| `/.rockbox/BCM4343A1.hcd` | BCM4343A1 patchram (~50 KB), extracted from stock Linux firmware |
| `/.rockbox/bt_keys.dat` | BTstack TLV link-key DB (8 slots × 32 B + header, magic `BTV_`) |
| `/.rockbox/bt_bonded.dat` | Bonded-device metadata: addr + name, MRU-ordered, up to 8 entries (magic `BTB1`) |
| `/.rockbox/bt_last.dat` | **Legacy** — single-device pairing record (magic `BTL1`). Auto-migrated to `bt_bonded.dat` on first boot under C9; left in place for one boot in case of rollback |
| `/.rockbox/bt_keys.dat` | Persistent link keys (~320 bytes, magic `BTV_`, 8 slots × 32 B) |
| `/.rockbox/bt_link.log` | Link-event log (append-on-pause, 256 KB cap, `=== log start ===` session markers). The primary diagnostic surface — the Redmi bug was diagnosed entirely from this file |

## Build & deploy

```bash
cd /home/avery/git_projects/rockbox/build-erosqnative
make -j$(nproc)
make zip
unzip -o rockbox.zip -d /run/media/avery/F20/
sync
```

Device mounts at `/run/media/avery/F20/` (FAT32). Run `make zip` so the toolchain produces `rockbox.zip` containing the full `.rockbox/` tree; unzipping over the device replaces all firmware artifacts atomically.

To re-extract the BCM patchram from a fresh stock firmware dump, see the artifacts list at the bottom of this file.

## What changed since 2026-07-14 (C13 — outdoor RF robustness, 2026-07-22)

Driven by the "works indoors, nearly unusable walking outdoors" BFP problem. The diagnosis, from the outdoor-walk link log + hardware research:

- **Physics**: indoors, wall/ceiling reflections fill the null when the body blocks the F20→bud path; outdoors there are no reflectors, so a body-shadowed path just loses 20–40 dB. Everyone's earbuds degrade outdoors — the F20 falls off a cliff because it has far less link margin than a phone.
- **Hardware**: the F20 is the weak end both directions. BCM4343A1 = BT 4.1-era WiFi/BT combo chip, **single shared antenna**, no-name module (`BAW_NM372SM`), *generic* patchram (the `.hcd` version string says so — generic RF calibration, not per-board). The BFP is Class 1 BT 5.0 on Apple H1 — not the problem. Stock-firmware Surfans reviews report the same outdoor spottiness, so this predates our stack. TX power is pinned 12/12 dBm (probe confirms every 4 s) — **no headroom lever exists**.
- **Log signatures**: send-gap p50 70 ms / p99 480 ms during the walk; `slots max=1` (LM downgrading to 27-byte 1-slot packets = throughput collapse); stalls of 1.7 s / 7.9 s / **21 s with the ACL alive** (supervision doesn't care about media — gotcha 37). Also: the ring log dropped thousands of lines between flushes on the walk sessions — enlarge the ring if outdoor diagnosis continues.

Levers landed (one commit each):

- **Adaptive AAC bitrate** (`bt-pcm-sink.c`) — 3 send gaps >200 ms inside 10 s → downshift one ladder step (128→96→64 k) mid-stream, no renegotiation (gotcha 36); 60 s with no distress → recover one step toward the negotiated ceiling. Adapted rate survives suspend/resume; fresh negotiation resets it. Log: `AAC rate X -> Y (gaps|clean)`.
- **Link probe** (`bt-service.c`) — the TX-power probe now chains `Read_RSSI` + `Read_Link_Quality`; one `link tx=12/12 rssi=R lq=Q` line per 4 s round replaces the old `txpow` line (gotcha 38). This is the margin data for tuning the adaptive thresholds.
- **Stall watchdog** (`bt-service.c` + `bt_pcm_sink_stall_ms()`) — ≥5 s with no successful media send while streaming → pending-switch to the same device + `gap_disconnect` (HCI-level on purpose — gotcha 37) → auto-reconnect. One kick per 30 s max. Log: `stall N ms -> re-conn`. Turns a 20 s freeze into a ~2–3 s blip.

Ruled out (again/for good): TX power vendor hacks (at ceiling), EDR re-enable (outdoors is a *worse* SNR regime than the body-null tests that killed it), AFH tricks (WiFi interference is an indoor problem; outdoors is pure path loss). The no-code lever that remains: carry the F20 high on the body, same side as the connected bud — a pants pocket outdoors is 50+ cm of tissue with no reflections, beyond what any firmware can fix.

Also verified this session (user report, 2026-07-22): the C12b firmware works end-to-end on hardware; the caps-intersection log line fires. `sel SBC` has still never appeared in any log — the SBC selection path remains hardware-unexercised.

## What changed since 2026-06-08 (C12b — AAC negotiation hardening, 2026-07-14)

A code-review pass over the AAC work, driven by a live bug: the Redmi speaker paired, showed "connected", Rockbox kept playing — and no sound came out. The on-device link log nailed it without any new instrumentation: Redmi sessions showed `sel AAC rc=0` and then *nothing* (no `cfg AAC`, zero packets, ACL alive with txpow probes ticking), while BFP sessions show `cfg AAC` within ~1 s. The sink never accepted our AAC SET_CONFIGURATION and nothing handled that.

Fixes landed (one commit each):

- **Sink-caps intersection + SBC fallback on reject/timeout** — the root-cause fix. See gotchas 31 and 32.
- **AAC encoder rebuilt on resume-after-suspend** — gotcha 33.
- **Oversize-frame-vs-MTU drop with distinct logging** — gotcha 34.
- **AAC caps advertise 44.1 kHz only** — gotcha 35.
- **Robustness batch**: `default_event_handler()` in the BT screens (USB plug / poweroff were swallowed, worst in the blocking actions menu), TX-power probe guarded by `hci_can_send_command_packet_now()`, and the foreground→BT-thread command mailbox replaced with an 8-deep ring queue (two quick UI actions could silently overwrite each other; disable is now a level-triggered flag so its blocking caller can't hang on a dropped command).

Also new (uncommitted work folded in the same session): the MENU actions menu replacing the long-press-forget gesture, and a `HCI_EVENT_PIN_CODE_REQUEST` → "0000" handler so legacy (pre-SSP) sinks can pair.

**Not yet verified on hardware**: reconnect the Redmi and check the log — expect `AAC caps ot=.. sf=.. ch=..` followed by either a working AAC stream, an immediate `sel SBC` (caps intersection said no), or `AAC cfg rej/timeout -> SBC retry` and a working SBC stream. Note the SBC selection path (`sel SBC`) had *never executed on hardware* before this — every logged session since the AAC work landed picked AAC.

## What changed since 2026-05-03 (C12 — AAC source codec, 2026-06-08)

The BFP walking stutter drove a codec change: Apple H1 sinks buffer AAC much more deeply than SBC, riding out the RF retransmit stalls that tore SBC apart in the pocket. Three pieces:

- **Codec selection**: btstack's default a2dp_source auto-config only ever picks SBC. `ENABLE_A2DP_EXPLICIT_CONFIG` + capability collection in bt-service, committing the choice at CAPABILITIES_COMPLETE (AAC preferred, SBC fallback).
- **Encoder**: vo-aacenc (fixed-point, caller-supplied memory operator backed by a static 64 KB arena — measured high-water 48928 B — no libm, no malloc, fine on the FPU-less X1000). FAAC was tried first and abandoned (float path).
- **Wire format**: LATM AudioMuxElement(MCP1) per RTP packet — gotcha 29. Raw AUs decode to silence on Apple sinks.

Plus, in the same arc: SBC 15-frame packet cap (gotcha 30), the "Audio quality" setting (128/96/64 kbps AAC, capped to the sink's advertised max; 64k = "pocket mode" — smallest frames, fastest retransmit recovery), DH3 slot cap + stay-master under AAC (worst send-gap tear ~270 ms vs 1760 ms+ prior), append-on-pause link logging, and the read-only TX-power probe (verdict: cur==max==12 dBm — TX is pinned at its ceiling, so frame size is the only robustness lever left for the pants-pocket case). Reorganized Bluetooth menu (Devices / Audio quality / Link logging / Auto-connect / Turn off). Auto-reconnect hijack guard: while an explicit connect/switch is in flight, a *different* device's incoming connection is rejected so it can't steal the session (Apple buds auto-reconnect instantly — this was the "had to forget the device to switch" bug).

New settings: `bt_aac_bitrate`, `bt_link_logging`.

## What changed since 2026-04-27 (C10b — Apple H1 sink hardening)

After C10 landed, end-to-end was working great with Bose Soundlink and a Redmi speaker (smooth, no glitches), but Beats Fit Pro produced periodic blips/disconnects every 20-40 s in worst cases. Bose/Redmi as a control showed the audio path itself is healthy — the issue is specifically in how Apple H1-class earbuds tolerate non-Apple sources. Two SBC-only-source quirks of Apple gear:

- **They prefer AAC and run SBC as a fallback.** Their SBC decoder is tuned conservatively; high bitpool stresses it more than it would a "first-class SBC" sink like Bose. Lowering the advertised `max_bitpool` from 53 → 35 (`bt-service.c` `sbc_caps[3]`) shrinks each on-air frame, cuts retransmit cost, and dropped Beats glitches from "every 20-40 s" to "~4 in 10 minutes."
- **They aggressively try to enter sniff mode for battery.** Each active↔sniff transition stalls media for ~100 ms. Calling `gap_set_default_link_policy_settings(LM_LINK_POLICY_DISABLE_ALL_LM_MODES)` tells our local LM to refuse sniff/hold/park requests from the peer, keeping the link in active mode for the duration of the stream.

Bose / Redmi sinks are unaffected by either change (they were already running at lower negotiated bitpools and never tried sniff mid-stream).

Possible future knobs if hitches persist:
- ~~Finite L2CAP flush timeout (~30 ms) on the AVDTP media channel.~~ Tried 2026-04-29 (30 ms) and 2026-05-03 (50 ms); both caused the Beats Fit Pro to disconnect ~5 s after every connect. See gotcha 25. Don't enable for Apple H1 sinks.
- ~~Force basic-rate-only ACL packet types (no EDR).~~ **Done in C10c (2026-05-03).** Big win for body-blocking on Beats Fit Pro.
- ~~BCM4343A1 TX power bump via Broadcom vendor command.~~ Ruled out 2026-06-08: the read-only TX-power probe (C12) shows cur==max==12 dBm — the controller is already pinned at its ceiling, zero headroom.
- ~~AAC source codec.~~ **Done in C12 (2026-06-08)** via vo-aacenc (not fdk-aac — fixed-point and malloc-free mattered more than encoder quality).

## What changed since 2026-04-22 (audio quality bring-up — C6b)

The pre-2026-04-22 build had end-to-end BT audio reaching the speaker, but the audio was severely choppy ("half-second spurts every half-second"). The fix combined three things, all required:

- **Dedicated BT thread (C6b).** The BTstack run loop now lives in its own kernel thread at `PRIORITY_PLAYBACK_MAX` (5). Foreground (`dbg_bt_a2dp`) shrinks to UI-only. `hal_cpu_enable_irqs_and_sleep` sleeps on a wakeup semaphore signaled by the UART RX ISR.
- **UART switched to 3 Mbps.** Manual `0xFC18` HCI vendor command after patchram + 50 ms settle on each side + RX drain + sanity probe (`HCI_Read_Local_Version`). `cfg.baudrate_init` set to `3000000` so BTstack doesn't reset us back to 115200 when opening the transport.
- **`AUDIO_TIMEOUT_MS = 1`.** Compensates for our 10 ms `hal_time_ms` granularity so the audio_tick fires at ~100 Hz instead of ~50 Hz.

Result: clean 44.1 kHz SBC streaming, ~100 packets/s, no underruns.

## What changed since 2026-04-16

The pre-04-16 doc concluded "PC22 (BT_PWR) module main power was the missing piece" after extensive disassembly of the kernel's `bcm_pm_core` driver. That conclusion was incorrect for HW4. Subsequent testing revealed:

- **PC22 is not required**. The current `bt-erosqnative.c` does not drive PC22 at any point; the chip works without it. Module main power on HW4 comes from a regulator rail rather than a host GPIO.
- **PB26 CLK32K is required**. The actual missing piece was the 32.768 kHz LPO reference. Without this, the chip's ROM does not run; CTS stays HIGH because the chip's RTS pin floats at its default (not because the chip is "rejecting" anything).
- **The "chip is fully off" hypothesis was right; the cause was wrong**. The pre-04-16 doc correctly diagnosed that the BT ARM core wasn't running. It just attributed that to missing main power instead of missing reference clock.
- **UART pin map (PC10-13 func0) was correctly identified** in the 04-16 corrections. That part stuck.

The kernel driver disassembly work (`bcm_pm_core` regulator, `bluesleep` ioctl) was useful for confirming the *order* of bring-up steps but pointed at the wrong missing signal.

## Reference: stock firmware artifacts

For re-extracting reference materials from a fresh NAND dump:

| Artifact | Source | Purpose |
|----------|--------|---------|
| `/lib/firmware/BCM4343A1_001.002.009.0122.0538.hcd` | UBI rootfs | Patchram blob |
| `/usr/bin/bt_init` | UBI rootfs | Stock init script (reference) |
| `/usr/bin/brcm_patchram_plus` | UBI rootfs | Linux patchram tool (reference) |
| Linux kernel | `Ingenic-flash-dump/of_player.img` | Disassembly reference for power sequencing |

Re-extract:
```bash
ubireader_extract_files Ingenic-flash-dump/flash.img -o /tmp/ubi-files/
# then copy /tmp/ubi-files/0/rootfs/lib/firmware/BCM4343A1*.hcd to .rockbox/

# Decompress kernel (gzip with 64-byte header)
python3 -c "open('/tmp/dtb_extract/kernel_gzip.bin','wb').write(
    open('Ingenic-flash-dump/of_player.img','rb').read()[64:])"
zcat /tmp/dtb_extract/kernel_gzip.bin > /tmp/dtb_extract/vmlinux.bin
```

## Toolchain

- Cross-compiler: `mipsel-elf-gcc (GCC) 9.5.0` at `/usr/local/mipsel-elf/`
- Disassembler: `mipsel-elf-objdump` at `/usr/local/bin/mipsel-elf-objdump`
