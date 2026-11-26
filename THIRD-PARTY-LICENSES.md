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
| `earlephilhower/ESP8266Audio` | `^2.4.3` | GPL-3.0-or-later | MP3/WAV decoder & audio output; includes `libmad` (GPL-2.0-or-later) |

## Platform & Framework Components (`espressif32` / Arduino)

- **arduino-esp32** core: LGPL-2.1
- **ESP-IDF / mbedTLS**: Apache-2.0
- **FreeRTOS**: MIT
- **lwIP**: BSD-3-Clause
- **FatFs**: FatFs License (permissive)
- **Espressif Wi-Fi / PHY binary blobs**: Espressif proprietary binary libraries

## Distribution Notice

Because compiled firmware statically links `ESP8266Audio` (**GPL-3.0-or-later**) and `LibSSH-ESP32` (**LGPL-2.1-or-later**), any compiled binaries distributed to third parties are subject to **GPL-3.0** terms (including corresponding source obligations). This repository distributes source code only.
