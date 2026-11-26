#pragma once

#include <stdint.h>

namespace cfg {

// Cardputer-Adv microSD SPI pins
constexpr int SD_SCK  = 40;
constexpr int SD_MISO = 39;
constexpr int SD_MOSI = 14;
constexpr int SD_CS   = 12;

// Internal LittleFS storage paths
constexpr const char* PATH_HOSTS_CSV   = "/hosts.csv";
constexpr const char* PATH_WIFI_CSV    = "/wifi.csv";
constexpr const char* PATH_KNOWN_CSV   = "/known_hosts.csv";
constexpr const char* PATH_KEYS_DIR    = "/keys";
constexpr const char* PATH_SETTINGS    = "/settings.cfg";
constexpr const char* FILE_ALARMS      = "/alarms.cfg";

// microSD import/export paths (relative to SD root)
constexpr const char* SD_DIR_SSH        = "/ssh";
constexpr const char* SD_DIR_KEYS       = "/ssh/keys";
constexpr const char* SD_DIR_LOGS       = "/ssh/logs";
constexpr const char* SD_DIR_RECORDINGS = "/recordings";
constexpr const char* SD_DIR_MUSIC      = "/music";
constexpr const char* SD_DIR_NOTES      = "/notes";
constexpr const char* SD_HOSTS_CSV      = "/ssh/hosts.csv";
constexpr const char* SD_WIFI_CSV       = "/ssh/wifi.csv";
constexpr const char* SD_README_MD      = "/README.md";

// Display geometry (240x135)
constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;
constexpr int TERM_COLS = 40;
constexpr int TERM_ROWS = 15;
constexpr int TERM_BAR_H = 15; // 15 + 15*8 = 135 px

// UI RGB565 palette
constexpr uint16_t COL_BG     = 0x0000;
constexpr uint16_t COL_DARK   = 0x18E3; // ~#1F1F1F
constexpr uint16_t COL_ORANGE = 0xCBAB; // #CC785C
constexpr uint16_t COL_ACCENT = COL_ORANGE;
constexpr uint16_t COL_CREAM  = 0xEF7D; // #F0EEE6
constexpr uint16_t COL_GRAY   = 0x7BEF; // #777777
constexpr uint16_t COL_DIM    = 0x3186; // #333333
constexpr uint16_t COL_GREEN  = 0x07E0;
constexpr uint16_t COL_RED    = 0xF800;
constexpr uint16_t COL_CYAN   = 0x07FF;
constexpr uint16_t COL_YELLOW = 0xFFE0;
constexpr uint16_t COL_WHITE  = 0xFFFF;

}  // namespace cfg
