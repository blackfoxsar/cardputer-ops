# Third-Party Licenses

This repository's own source code (`src/`, configuration, and templates) is licensed under the **Apache License, Version 2.0** (see [`LICENSE`](LICENSE)).

When building the firmware with PlatformIO, external third-party libraries and framework components are fetched and statically linked into the compiled binary. No prebuilt firmware binaries are distributed in this repository.

## Library Dependencies (`platformio.ini`)

| Library | Version / Spec | License | Notes |
|---|---|---|---|
| `m5stack/M5Cardputer` | `^1.1.1` | MIT | Cardputer / Cardputer-Adv hardware support |
| `m5stack/M5Unified` | latest | MIT | Unified M5Stack hardware abstraction |
| `m5stack/M5GFX` | latest | MIT | Display & graphics library |
| `IRremote` | transitive (via `M5Cardputer`) | MIT | Pulled transitively by `M5Cardputer`; not directly included in `src/` |
| `ewpa/LibSSH-ESP32` | `libssh 0.11.5` | LGPL-2.1-or-later | SSH client engine statically linked in `ssh_engine.cpp` |

## Vendored Source (`src/third_party/`)

| Component | Version | License | Notes |
|---|---|---|---|
| `dr_mp3` by David Reid | v0.7.5 | Public Domain (Unlicense) or MIT-0 | Single-header MP3 decoder vendored at `src/third_party/dr_mp3.h`; source: https://github.com/mackron/dr_libs (based on `minimp3`, CC0) |

## Platform & Framework Components (`espressif32` / Arduino)

- **arduino-esp32** core: LGPL-2.1
- **ESP-IDF / mbedTLS**: Apache-2.0
- **FreeRTOS**: MIT
- **lwIP**: BSD-3-Clause
- **FatFs**: FatFs License (permissive)
- **Espressif Wi-Fi / PHY binary blobs**: Espressif proprietary binary libraries

## Distribution Notice

- No GPL-licensed code is linked into the firmware.
- `LibSSH-ESP32` (**LGPL-2.1-or-later**) and the arduino-esp32 core (**LGPL-2.1**) are statically linked. The LGPL relinking requirement is satisfied because the complete application source and `platformio.ini` are provided, allowing users to rebuild and relink against modified versions of these libraries.
- Espressif Wi-Fi / PHY binary blobs are redistributable under Espressif's license terms.
- This repository distributes source code only.
