<div align="center">

<img src="docs/assets/logo_airplay_esp32.png" alt="AirPlay ESP32" width="400">

# ESP32 AirPlay 2 Receiver

**Stream music from your Apple devices — or from any phone over Bluetooth — to any speaker for about $10**

[![GitHub stars](https://img.shields.io/github/stars/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/rbouteiller/airplay-esp32?style=flat-square)](https://github.com/rbouteiller/airplay-esp32/network)
[![License](https://img.shields.io/badge/license-GPL--3.0--or--later-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1+-red?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Platform](https://img.shields.io/badge/platform-ESP32%20%7C%20S2%20%7C%20S3%20%7C%20C5-green?style=flat-square)](https://www.espressif.com/en/products/socs)

### [Documentation](https://rbouteiller.github.io/airplay-esp32/) · [Install from your browser](https://rbouteiller.github.io/airplay-esp32/getting-started/flashing/) · [Troubleshooting](https://rbouteiller.github.io/airplay-esp32/troubleshooting/)

</div>

---

## About this fork: the `livelab` branch

This branch adapts the firmware to the LiveLab ESP32-WROVER-E board (TOSLINK optical
transmitter + 3.5mm DAC) and adds a **DLNA / UPnP AV media renderer**, so one board takes
AirPlay 2, Bluetooth A2DP and DLNA, and plays any of them bit-perfect over optical S/PDIF
or through the analog output. The main use case is casting lossless FLAC from NetEase
Cloud Music; other DLNA control points work too.

### What it adds

- **DLNA renderer** (`main/livelab/`): SSDP discovery, AVTransport / RenderingControl /
  ConnectionManager over SOAP, GENA eventing. Pulls the stream over HTTP into a 1.5 MB
  PSRAM buffer with seeking.
  - Formats: FLAC (up to 96 kHz / 24-bit), WAV, MP3 and M4A (AAC-LC). The format is
    detected from the file content, not the Content-Type (NetEase mislabels FLAC and WAV
    as `audio/mpeg`).
  - Built for control points that rely on events rather than polling: subscriptions are
    kept when a phone is temporarily unreachable, and a re-subscribe from the same callback
    keeps its SID.
  - Survives restarts: subscriptions and the current track are stored in NVS. Before a
    restart or OTA the renderer pauses and tells the control point; after boot a plain
    Play resumes at the saved position.
- **Unified 24-bit output** (`output.c`): one task owns I2S. You can switch between
  optical S/PDIF (software BMC encoding on IO14, APLL clock) and the 3.5mm DAC during
  playback. A sample-rate change mutes briefly so the receiver can relock.
- **Source preemption** (`livelab.c`): the newest source wins. Starting DLNA pauses
  AirPlay and disconnects Bluetooth, and the other way round. All three stay discoverable.
- **S/PDIF fixes** (`audio_output_spdif.c`):
  - Adds the missing stop / write / set-rate hooks.
  - Each DMA buffer is a whole 192-frame block with the B preamble at a fixed position,
    so underruns no longer make the receiver lose lock.
- **Other fixes**: a 64 KB Bluetooth ring buffer in PSRAM, and web log frames split on
  UTF-8 boundaries.
- **Web UI** (Chinese / English) at `http://<board-ip>/`:
  - Now playing, with format, bit rate, buffer and decoder load.
  - Transport controls, volume and output selection.
  - Settings, OTA upload and live logs.
- **QEMU simulator** (`sim/`): runs the same firmware with the test script acting as
  control point, media server and receiver, and compares audio sample by sample. It has
  19 scenarios.
- **WiFi-only maintenance**: after a one-time UART install, everything is updated over WiFi
  and a bad image cannot brick the board (see [Recovery](#recovery)).

### Board

| Signal | GPIO |
|---|---|
| Optical (TOSLINK Vin) | 14 |
| DAC MCLK / BCK / LRCK / DATA | 0 / 33 / 32 / 25 |

IO0 is both MCLK and the boot-mode pin. Switch the output back to optical in the web UI
before pulling IO0 low to flash over UART.

### Build, flash, test

```bash
source ~/esp/esp-idf-v6.1/export.sh
# With Bluetooth (drop the .bt layer for a build without it)
idf.py -B build_ll -D SDKCONFIG=build_ll/sdkconfig \
  -D SDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.bt;config/sdkconfig.defaults.livelab" build

# OTA over WiFi (about 15 s), restart, live log
curl -X POST --data-binary @build_ll/airplay2-receiver.bin http://<board-ip>/api/ota/update
curl -X POST http://<board-ip>/api/system/restart
# WebSocket log stream: ws://<board-ip>/ws/logs

# Recovery app (factory partition), real board and simulator variants
idf.py -C recovery -B $PWD/build_recovery -D SDKCONFIG=$PWD/build_recovery/sdkconfig build
idf.py -C recovery -B $PWD/build_recovery_sim -D SDKCONFIG=$PWD/build_recovery_sim/sdkconfig \
  -D SDKCONFIG_DEFAULTS="$PWD/recovery/sdkconfig.defaults;$PWD/recovery/sdkconfig.sim" build

# Simulator: build the sim variant, generate media once, run all scenarios
idf.py -B build_sim -D SDKCONFIG=build_sim/sdkconfig \
  -D SDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.livelab;config/sdkconfig.defaults.sim" build
sim/make_media.sh
python3 sim/test_sim.py            # or: python3 sim/test_sim.py m4a reboot
```

Run the simulator with the system Python, not inside the `export.sh` shell. It needs
`numpy` and `ffmpeg`. QEMU hangs while writing flash during OTA, so the `ota` scenario
stops after the image has been received and verified.

### Recovery

Partition table `components/boards/partitions_livelab.csv`: nvs, otadata, `ota_0` and `ota_1`
(3 MB each), `factory` (1 MB, the recovery app) and SPIFFS.

- **Rollback**: a newly flashed image boots as "pending verify" and marks itself valid once
  the network, web UI and OTA endpoint are up. If it resets before that, the bootloader
  goes back to the previous image.
- **Boot-loop guard** (`bootloader_components/livelab_boot_hooks`): the bootloader counts
  boots in `RTC_CNTL_STORE0`. The count is cleared on power-on, and by the firmware after
  60 s of uptime. On the 5th boot in a row without a healthy run, the bootloader erases
  otadata and starts the factory partition.
- **Recovery app** (`recovery/`): opens the open hotspot `ESP32-Recovery` (192.168.4.1)
  right away. It also tries the saved WiFi for up to 60 s, and stops retrying if that
  fails, because STA retries make the hotspot unstable.
  - It serves a firmware upload page and the same `POST /api/ota/update` endpoint, so the
    usual curl command works.
  - It can boot an existing image again: `POST /api/boot?part=ota_0`.
  - The main firmware's System page has a **Recovery mode** button
    (`POST /api/ll/recovery`).
- **Button**: hold the on-board button (GPIO15, low when pressed) while powering on, for
  about 5 s. The bootloader erases otadata and starts the recovery app, even if both
  firmware images are broken (`CONFIG_BOOTLOADER_FACTORY_RESET`). No data partition is
  erased. It is disabled in the simulator, because QEMU does not emulate pull-ups.
- The recovery app is never updated over the air. Only the bootloader, the partition
  table and the recovery app need UART, once.
- To test it, build with `CONFIG_LIVELAB_BOOT_TEST`:
  - `=1` crashes before marking itself valid: expect a rollback.
  - `=2` crashes 10 s after every boot: expect recovery mode.
  - `=3` hides the saved WiFi and restarts into recovery: expect the hotspot. Booting
    the test image again from recovery restores the WiFi.

Everything below is the upstream README.

---

## What is this?

This turns a cheap ESP32 board into a wireless AirPlay 2 speaker. Plug it into any
amplifier or powered speakers and it shows up on your iPhone, iPad or Mac just like a
HomePod or an AirPlay TV.

Works with **ESP32**, **ESP32-S2**, **ESP32-S3** and **ESP32-C5** chips, including the
[SqueezeAMP](https://github.com/philippe44/SqueezeAMP) (ESP32 + TAS5756) and
[Esparagus Audio Brick](https://sonocotta.com/espragus-audio-brick/) (ESP32 + TAS5825M)
boards, which have amplifiers built in.

ESP32-based boards also support **Bluetooth A2DP**, so anything that can pair with a
Bluetooth speaker can play to them when AirPlay is idle. The Esparagus Audio Brick
additionally supports **wired Ethernet** through an optional W5500 module.

**No cloud. No app. Just tap and play.**

## Quick start

The fastest route is the browser installer — no toolchain, no command line:

**[→ Install from your browser](https://rbouteiller.github.io/airplay-esp32/getting-started/flashing/)**

Building from parts instead? You need an ESP32-S3 dev board, a PCM5102A DAC and a female
pin header, roughly $10 total, and no soldering. See the
[getting started guide](https://rbouteiller.github.io/airplay-esp32/getting-started/).

Building from source:

```bash
git clone --recursive https://github.com/rbouteiller/airplay-esp32
cd airplay-esp32
pio run -e esp32s3 -t upload
pio run -e esp32s3 -t uploadfs   # required — writes the web UI to SPIFFS
```

## Features

- **AirPlay 2** — appears natively in Control Center and every AirPlay-capable app
- **ALAC and AAC decoding** — live streaming (Siri, calls) and music playback
- **Multi-room** — PTP-based timing for synchronised playback
- **Bluetooth A2DP** — receive audio from phones and tablets (ESP32 boards only)
- **W5500 Ethernet** — wired networking with automatic WiFi failover
- **Web configuration** and **OTA updates** — USB is only needed for the first flash
- **48 kHz output** — optional 44.1 → 48 kHz conversion via a sinc resampler
- **Displays** — optional OLED or 320×170 colour TFT with track metadata
- **Hardware buttons** — optional play/pause, volume and track skip

Audio only, one speaker per board, and a decent WiFi signal is required.

## Documentation

| | |
| --- | --- |
| [Getting started](https://rbouteiller.github.io/airplay-esp32/getting-started/) | Shopping list, assembly, flashing, first boot |
| [Supported boards](https://rbouteiller.github.io/airplay-esp32/boards/) | SqueezeAMP, Esparagus Audio Brick, XIAO ESP32-C5, custom boards |
| [Features](https://rbouteiller.github.io/airplay-esp32/features/bluetooth/) | Bluetooth, Ethernet, displays, buttons, AirPlay tuning |
| [Reference](https://rbouteiller.github.io/airplay-esp32/reference/build-environments/) | Build environments, SPIFFS, OTA, architecture |
| [Troubleshooting](https://rbouteiller.github.io/airplay-esp32/troubleshooting/) | No sound, no setup WiFi, dropouts, build errors |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and the
[contributing guide](https://rbouteiller.github.io/airplay-esp32/contributing/).
Documentation lives in [`docs/`](docs/) and is built with [Zensical](https://zensical.org/)
— every page on the site has an edit link that takes you straight to the GitHub editor.

## Acknowledgements

- [Shairport Sync](https://github.com/mikebrady/shairport-sync) — the reference AirPlay implementation
- [openairplay/airplay2-receiver](https://github.com/openairplay/airplay2-receiver) — Python AirPlay 2 implementation
- [Espressif](https://github.com/espressif) — ESP-IDF framework and codec libraries

## License

Copyright © 2026 Rémi Bouteiller and the airplay-esp32 contributors.

This program is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version. It is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE) for the
full text.

[LICENSE-EXCEPTION](LICENSE-EXCEPTION) grants the additional permission needed to link
this firmware against Espressif's closed binary components — notably the `esp_audio_codec`
ALAC and AAC decoders, whose license restricts use to Espressif chips. Without it, neither
this project nor anyone downstream could ship a working binary.
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) lists the licenses of everything else
that ends up in the firmware.

Building a product on this? You may, including commercially, provided you release your
source under the GPL as well.

## Legal

This is an independent project based on protocol analysis. Not affiliated with Apple Inc.
Not guaranteed to work with future iOS or macOS versions. Provided as-is without warranty.
