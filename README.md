# cardputer-ops

Native C++ all-in-one firmware for the **M5Stack Cardputer-Adv** (ESP32-S3, TCA8418 I2C matrix keyboard, ES8311 audio codec, 8 MB Flash), built with PlatformIO, `M5Cardputer`, and `LibSSH-ESP32` (`libssh 0.11.5` with mbedTLS hardware acceleration).

## Built-in Apps

The launcher uses a two-column grid (`1-7` hotkeys or `; . , /` navigation + `Enter`), with a persistent top-right status bar across every screen showing current time (`HH:mm`), Wi-Fi status icon, and battery percentage (`GPIO10`), plus the current date (`yyyy-MM-dd`) on the home screen. Most screens exit/go back uniformly via `ESC` (`Fn+\``) or `BtnG0`.

1. **SSH Client**
   - **Quick-connect host list**: Stores host profiles (Host, User, Password, Port, Key file, Legacy flag, Alias), showing `[L]`/`[K]`/`[P]` badges and `Enter:Go N:New E:Edit D:Del` hints.
   - **Terminal scrollback paging**: Keeps 120 lines of history; press `Opt+;` / `Opt+.` to page up/down locally without transmitting `Opt` sequences to the remote host.
   - **Legacy SSH compatibility toggle**: Configurable per host or as a global default in `SSH Settings` to allow `ssh-rsa`, `diffie-hellman-group14-sha1`, `diffie-hellman-group1-sha1`, `aes128/192/256-cbc`, and `hmac-sha1`.
   - **SD terminal session logging**: When `SD Session Log` is enabled, ANSI escape sequences are stripped and sessions are written to `/ssh/logs/<host-or-ip>/yyyyMMdd_HHmmss.log` on microSD.
   - **Key-based authentication**: Generate **Ed25519**, **ECDSA P-256**, or **RSA-2048** key pairs on-device (saved to internal LittleFS `/keys/`). Press `E` on a key to export it (private+`.pub` or `.pub` only) to `/sd/ssh/keys/`, or press `I` in the key list to import individual or all keys from `/sd/ssh/keys/`.
   - **Bidirectional LittleFS <-> SD CSV sync**: Import or export `hosts.csv`, `wifi.csv`, and `keys/*` via `/sd/ssh/`.
   - **Password visibility toggle**: Press `Tab` in any password or passphrase field to toggle plain-text display vs. masked asterisks.
2. **Net Tools >**
   - **Ping**: Continuous ICMP Echo ping (with TTL and RTT) or TCP port ping (`host:port`) with Tx/Rx/Loss%/Avg stats.
   - **Dig (DNS Lookup)**: UDP/53 DNS query tool supporting `A`, `AAAA`, `CNAME`, `MX`, `TXT`, `NS`, `PTR` records against system DNS or a custom server.
   - **MTR (Traceroute)**: Live hop-by-hop ICMP TTL traceroute showing per-hop IP, loss %, and average RTT.
3. **Voice Rec**
   - Records 16 kHz 16-bit PCM WAV audio via the ES8311 microphone with a live level meter to `/recordings/yyyyMMdd_HHmmss.wav`, with built-in playback (1% volume steps with hold-to-accelerate), deletion, and clean ES8311 codec mute/idle teardown.
4. **Audio Playback**
   - Built-in **1 kHz Test Tone**: a pure 1000 Hz sine wave that works without an SD card, useful for speaker / audio-path debugging.
   - `.mp3` (decoded with `dr_mp3`) / `.wav` playback from `/music/` (kept separate from `/recordings/`), supporting ID3 title/artist display, pause/resume, previous/next track, fine 1% volume adjustment (hold for fast ramp), and repeat mode (`ALL` / `ONE` / `OFF`).
5. **Tools >**
   - **1. Alarm**: Configure up to 6 alarms (`HH:MM`, Daily/Once, custom label, adjustable volume) persisted in LittleFS `/alarms.cfg`, with background trigger + 5-minute snooze.
   - **2. dB Meter**: Real-time ES8311 microphone sound level meter showing dB SPL, Quiet/Moderate/Loud/High level, Min/Avg/Peak stats, rolling bar history, and adjustable calibration offset.
   - **3. Text Editor**: microSD file browser (defaulting to `/notes/`, able to traverse up to `/`) and multi-line text editor (`Fn` arrows, `Ctrl+S` save, unsaved-change prompt).
   - **4. Format SD Card**: Detects microSD card type/capacity, formats to **FAT32** after two-step `Y` -> `Enter` confirmation, and optionally initializes `/ssh`, `/ssh/keys`, `/ssh/logs`, `/recordings`, `/music`, `/notes`, and `/README.md`.
6. **Games >**
   - Includes **1. Snake** and **2. Sudoku**.
7. **Settings >**
   - **1. Wi-Fi**: 2.4 GHz scan with RSSI color coding, encryption badges, hidden SSID display (`<bssid>` when unsaved, `*<ssid>` when saved), saved network management (`S`), **DHCP** and **Static IP** (with automatic fallback prompt if DHCP times out), and connection details (`I` / `D` disconnect).
   - **2. Date & Time**: Live clock display with manual date (`YYYY-MM-DD`), time (`HH:MM[:SS]`), and timezone (`UTC-12` to `UTC+14`), custom NTP server, and NTP sync.

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

No GPL-licensed code is linked into the firmware. Third-party components remain under their respective licenses: **MIT** (`M5Cardputer`, `M5Unified`, `M5GFX`, etc.), **Public Domain / MIT-0** (`dr_mp3`, vendored in `src/third_party/`), and **LGPL-2.1-or-later** (`LibSSH-ESP32`, arduino-esp32 core). See [`THIRD-PARTY-LICENSES.md`](THIRD-PARTY-LICENSES.md) for details.
