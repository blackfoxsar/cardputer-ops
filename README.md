# cardputer-ops

Native C++ all-in-one firmware for the **M5Stack Cardputer-Adv** (ESP32-S3, TCA8418 I2C matrix keyboard, ES8311 audio codec, 8 MB Flash), built with PlatformIO, `M5Cardputer`, and `LibSSH-ESP32` (`libssh 0.11.5` with mbedTLS hardware acceleration).

## Built-in Apps

The launcher uses a two-column grid (`1-7` hotkeys or `; . , /` navigation + `Enter`), with a top-right status bar displaying current time (`HH:mm`), Wi-Fi status icon, and battery percentage (`GPIO10`):

1. **SSH Client**
   - **Quick-connect host list**: Stores host profiles (Host, User, Password, Port, Key file, Legacy flag, Alias), showing `[L]`/`[K]`/`[P]` badges and `Enter:Go N:New E:Edit D:Del` hints.
   - **Legacy SSH compatibility toggle**: Configurable per host or as a global default in `SSH Settings` to allow `ssh-rsa`, `diffie-hellman-group14-sha1`, `diffie-hellman-group1-sha1`, `aes128/192/256-cbc`, and `hmac-sha1`.
   - **SD terminal session logging**: When `SD Session Log` is enabled, ANSI escape sequences are stripped and sessions are written to `/ssh/logs/<host-or-ip>/yyyyMMdd_HHmmss.log` on microSD.
   - **Key-based authentication**: Generate **Ed25519**, **ECDSA P-256**, or **RSA-2048** key pairs on-device (saved to internal LittleFS `/keys/`). Press `E` on a key to export it (private+`.pub` or `.pub` only) to `/sd/ssh/keys/`, or press `I` in the key list to import individual or all keys from `/sd/ssh/keys/`.
   - **Bidirectional LittleFS <-> SD CSV sync**: Import or export `hosts.csv`, `wifi.csv`, and `keys/*` via `/sd/ssh/`.
   - **Password visibility toggle**: Press `Tab` in any password or passphrase field to toggle plain-text display vs. masked asterisks.
2. **Wi-Fi**
   - Offline boot by default (does not block startup waiting for Wi-Fi).
   - 2.4 GHz scan with RSSI color coding, encryption badges, hidden SSID display (`<bssid>` when unsaved, `*<ssid>` when saved), and saved network management (`S`).
   - Supports **DHCP** and **Static IP** (automatically prompts to switch to static IP if DHCP times out), IPv4/Gateway/DNS/RSSI inspection, and **TCP Ping** (`host:port` with TCP RST RTT measurement).
3. **Date & Time**
   - Live clock display with manual date (`YYYY-MM-DD`), time (`HH:MM[:SS]`), and timezone (`UTC-12` to `UTC+14`) configuration, custom NTP server, and manual or automatic NTP sync.
4. **SD Format**
   - Detects microSD card type and capacity, formats to **FAT32** after two-step `Y` -> `Enter` confirmation, and optionally initializes `/ssh`, `/ssh/keys`, `/ssh/logs`, `/recordings`, `/music`, and `/README.md` (`[X] Init folders & README.md`).
5. **Voice Rec**
   - Records 16 kHz 16-bit PCM WAV audio via the ES8311 microphone with a live level meter to `/recordings/yyyyMMdd_HHmmss.wav`, with built-in playback and deletion.
6. **Music**
   - Standalone `.mp3` / `.wav` player reading from `/music/` (kept separate from `/recordings/`), supporting ID3 title/artist display, pause/resume, previous/next track, volume adjustment, and repeat mode (`ALL` / `ONE` / `OFF`).
7. **Games**
   - Includes **1. Snake** and **2. Sudoku**.

## SD Card CSV Templates (`/sd/ssh/`)

Reference templates are provided in [`sd_template/ssh/`](sd_template/ssh/):

- **`/sd/ssh/hosts.csv`**:
  ```csv
  host,user,password,port,key_file,legacy,name
  192.168.1.10,ubuntu,,22,id_ed25519,0,Home Linux
  192.168.1.254,admin,secret123,22,,1,Legacy Switch
  ```
  Minimal 3-column rows (`host,user,password`) are also accepted (`port=22`, `legacy=0`).
- **`/sd/ssh/wifi.csv`**:
  ```csv
  ssid,password
  MyOfficeWiFi,your-wifi-password
  GuestOpenNetwork,
  ```
  Extended columns (`ssid,password,hidden,bssid,dhcp,ip,mask,gw,dns`) are also supported for hidden networks and static IP profiles.

## Build & Flash

Requires [PlatformIO Core](https://platformio.org/install/cli) (`pio`). Connect the Cardputer-Adv via USB-C and run:

```bash
# Build firmware
pio run

# Build and flash over USB
pio run -t upload

# Open serial monitor (115200 baud)
pio device monitor
```

If the board does not enter download mode automatically, hold **G0**, press and release **Reset**, then release **G0** before running `pio run -t upload`.

## Origin & Acknowledgements

This project originated from [`moremas/build-with-claude`](https://github.com/moremas/build-with-claude). The M5Stack Cardputer-Adv hardware used during development came from that event, and the Snake game concept and color palette retain inspiration from that project. All firmware code in this repository has been rewritten from scratch in native C++ using PlatformIO.

## License

This repository's own source code is licensed under the **Apache License, Version 2.0** — see [`LICENSE`](LICENSE).

Third-party libraries fetched during compilation (`M5Cardputer`, `M5Unified`, `M5GFX`, `LibSSH-ESP32`, `ESP8266Audio`, etc.) remain under their respective licenses; see [`THIRD-PARTY-LICENSES.md`](THIRD-PARTY-LICENSES.md) for details (note that `ESP8266Audio` is **GPL-3.0-or-later** and `LibSSH-ESP32` is **LGPL-2.1-or-later**).
