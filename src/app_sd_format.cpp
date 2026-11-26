#include "app_sd_format.h"

#include <SD.h>
#include <SPI.h>
#include <sd_diskio.h>

extern "C" {
#include <diskio.h>
#include <ff.h>
}

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "config.h"
#include "ui_common.h"

namespace {

std::string formatBytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        return buf;
    }
    if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return buf;
    }
    snprintf(buf, sizeof(buf), "%llu KB", static_cast<unsigned long long>(bytes / 1024ULL));
    return buf;
}

const char* cardTypeLabel(sdcard_type_t t) {
    switch (t) {
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SD";
        case CARD_SDHC: return "SDHC";
        default: return "Unknown";
    }
}

bool confirmEnterStep(bool init_layout) {
    auto& cv = ui::canvas();
    ui::waitKeyRelease();
    while (true) {
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Format SD Card");
        cv.setTextColor(cfg::COL_RED, cfg::COL_BG);
        cv.drawString("CONFIRM FORMAT", 10, 30);
        cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        cv.drawString("Press Enter to erase & format", 10, 50);
        cv.setTextColor(init_layout ? cfg::COL_GREEN : cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString(init_layout ? "Will create folders & README.md"
                                  : "Blank FAT32 (no folders)", 10, 68);
        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString("Any other key cancels", 10, 86);
        ui::drawHints("Enter FORMAT", "other cancel");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        if (k.enter) {
            ui::waitKeyRelease();
            return true;
        }
        if (k.esc || k.btn_g0 || k.del || k.ch != 0) {
            ui::waitKeyRelease();
            return false;
        }
        delay(25);
    }
}

bool formatCardFat32(uint64_t& out_total_bytes, int& out_fres) {
    out_fres = 0;
    SPI.begin(cfg::SD_SCK, cfg::SD_MISO, cfg::SD_MOSI, cfg::SD_CS);

    uint8_t pdrv = sdcard_init(cfg::SD_CS, &SPI, 25000000);
    if (pdrv == 0xFF) {
        SPI.end();
        out_fres = -2;
        return false;
    }

    DSTATUS st = disk_initialize(pdrv);
    if (st & STA_NOINIT) {
        out_fres = -1;
        sdcard_uninit(pdrv);
        SPI.end();
        return false;
    }

    char drv[3] = {static_cast<char>('0' + pdrv), ':', 0};
    size_t work_sz = std::max<size_t>(FF_MAX_SS, 4096);
    BYTE* work = static_cast<BYTE*>(malloc(work_sz));
    if (!work) {
        out_fres = -3;
        sdcard_uninit(pdrv);
        SPI.end();
        return false;
    }

    MKFS_PARM opt = {static_cast<BYTE>(FM_FAT32), 0, 0, 0, 0};
    FRESULT res = f_mkfs(drv, &opt, work, work_sz);
    if (res != FR_OK) {
        opt.fmt = static_cast<BYTE>(FM_ANY);
        res = f_mkfs(drv, &opt, work, work_sz);
    }
    out_fres = static_cast<int>(res);
    free(work);

    bool ok = false;
    if (res == FR_OK) {
        if (sdcard_mount(pdrv, "/sd_fmt", 2, false)) {
            uint64_t sectors = sdcard_num_sectors(pdrv);
            uint32_t sec_sz = sdcard_sector_size(pdrv);
            out_total_bytes = sectors * static_cast<uint64_t>(sec_sz ? sec_sz : 512);
            sdcard_unmount(pdrv);
            ok = true;
        }
    }

    sdcard_uninit(pdrv);
    SPI.end();
    return ok;
}

bool createDefaultSdLayout() {
    static const char* kSdReadmeText =
        "# Cardputer Ops - microSD Directory Guide\n"
        "\n"
        "This microSD card is organized for M5Stack Cardputer-Adv (Cardputer Ops).\n"
        "\n"
        "## Folder Structure\n"
        "\n"
        "- `/ssh/`\n"
        "  Root folder for SSH Client CSV imports/exports:\n"
        "  - `/ssh/hosts.csv`  : `host,user,password,port,key_file,legacy,name`\n"
        "  - `/ssh/wifi.csv`   : `ssid,password` (or extended 9-col format)\n"
        "- `/ssh/keys/`\n"
        "  SSH private keys and `.pub` public keys for import/export.\n"
        "- `/ssh/logs/`\n"
        "  Saved SSH terminal session logs (when `SD Session Log` is ON in SSH Settings).\n"
        "  Stored per host as `/ssh/logs/<host_or_alias>/yyyyMMdd_HHmmss.log`.\n"
        "- `/recordings/`\n"
        "  Voice Recorder output files saved as 16kHz 16-bit mono PCM WAV:\n"
        "  `/recordings/yyyyMMdd_HHmmss.wav`.\n"
        "- `/music/`\n"
        "  Audio Playback library folder. Put `.mp3` or `.wav` audio files here.\n"
        "  (Keep separate from `/recordings/` so voice memos and music stay organized.)\n";

    SPI.begin(cfg::SD_SCK, cfg::SD_MISO, cfg::SD_MOSI, cfg::SD_CS);
    if (!SD.begin(cfg::SD_CS, SPI, 25000000)) {
        SPI.end();
        return false;
    }

    bool ok = true;
    const char* dirs[] = {
        cfg::SD_DIR_SSH,
        cfg::SD_DIR_KEYS,
        cfg::SD_DIR_LOGS,
        cfg::SD_DIR_RECORDINGS,
        cfg::SD_DIR_MUSIC,
    };
    for (const char* d : dirs) {
        if (!SD.exists(d)) {
            if (!SD.mkdir(d)) ok = false;
        }
    }

    File f = SD.open(cfg::SD_README_MD, FILE_WRITE);
    if (f) {
        size_t len = std::strlen(kSdReadmeText);
        if (f.write(reinterpret_cast<const uint8_t*>(kSdReadmeText), len) != len) {
            ok = false;
        }
        f.flush();
        f.close();
    } else {
        ok = false;
    }

    SD.end();
    SPI.end();
    return ok;
}

}  // namespace

void runSdFormatApp() {
    ui::waitKeyRelease();

    auto& cv = ui::canvas();
    cv.fillScreen(cfg::COL_BG);
    ui::drawHeader("Format SD Card");
    cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
    cv.drawString("Detecting SD card...", 48, 56);
    ui::drawHints("Please wait");
    cv.pushSprite(0, 0);

    bool detected = false;
    bool fs_readable = false;
    uint64_t card_bytes = 0;
    sdcard_type_t ctype = CARD_NONE;

    SPI.begin(cfg::SD_SCK, cfg::SD_MISO, cfg::SD_MOSI, cfg::SD_CS);
    if (SD.begin(cfg::SD_CS, SPI, 25000000)) {
        detected = true;
        fs_readable = true;
        ctype = SD.cardType();
        card_bytes = SD.cardSize();
        SD.end();
    } else {
        uint8_t pdrv = sdcard_init(cfg::SD_CS, &SPI, 25000000);
        if (pdrv != 0xFF) {
            DSTATUS st = disk_initialize(pdrv);
            if (!(st & STA_NOINIT)) {
                ctype = sdcard_type(pdrv);
                uint64_t secs = sdcard_num_sectors(pdrv);
                uint32_t sec_sz = sdcard_sector_size(pdrv);
                card_bytes = secs * static_cast<uint64_t>(sec_sz ? sec_sz : 512);
                detected = (ctype != CARD_NONE && ctype != CARD_UNKNOWN);
            }
            sdcard_uninit(pdrv);
        }
    }
    SPI.end();

    if (!detected) {
        ui::showToast("Format SD Card", "SD card not detected", "Insert microSD and try again", cfg::COL_RED);
        return;
    }

    ui::waitKeyRelease();
    bool init_layout = true;
    bool confirmed = false;
    while (true) {
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Format SD Card");
        if (fs_readable && card_bytes > 0) {
            cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            std::string l1 = std::string(cardTypeLabel(ctype)) + " card: " + formatBytes(card_bytes);
            cv.drawString(l1.c_str(), 10, 26);
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString("Filesystem looks readable", 10, 42);
        } else {
            cv.setTextColor(cfg::COL_YELLOW, cfg::COL_BG);
            std::string l1 = std::string(cardTypeLabel(ctype)) + " card detected";
            if (card_bytes > 0) {
                l1 += ": " + formatBytes(card_bytes);
            }
            cv.drawString(l1.c_str(), 10, 26);
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString("Filesystem unreadable / raw", 10, 42);
        }
        cv.setTextColor(cfg::COL_RED, cfg::COL_BG);
        cv.drawString("ALL DATA WILL BE LOST!", 10, 58);

        cv.setTextColor(init_layout ? cfg::COL_GREEN : cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString(init_layout ? "[X] Init folders & README.md"
                                  : "[ ] Init folders & README.md", 10, 78);

        cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        cv.drawString("Format as FAT32?", 10, 96);
        ui::drawHints("Y next  Space opt", "N / Q cancel");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'n' || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        if (k.ch == ' ' || lc == 'c') {
            init_layout = !init_layout;
        } else if (lc == 'y') {
            confirmed = confirmEnterStep(init_layout);
            break;
        }
        delay(25);
    }

    if (!confirmed) return;

    cv.fillScreen(cfg::COL_BG);
    ui::drawHeader("Format SD Card");
    cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
    cv.drawString("Formatting FAT32...", 10, 44);
    cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
    cv.drawString("Do not remove SD card", 10, 64);
    ui::drawHints("Working...");
    cv.pushSprite(0, 0);

    uint64_t formatted_bytes = 0;
    int fres = 0;
    if (formatCardFat32(formatted_bytes, fres)) {
        bool layout_ok = true;
        if (init_layout) {
            layout_ok = createDefaultSdLayout();
        }
        std::string l1 = "FAT32 ready" + (formatted_bytes ? (" (" + formatBytes(formatted_bytes) + ")") : "");
        std::string l2 = init_layout
                             ? (layout_ok ? "Created folders & /README.md" : "Folder/README write failed")
                             : "";
        ui::showToast("Format Complete", l1, l2,
                      (init_layout && !layout_ok) ? cfg::COL_YELLOW : cfg::COL_GREEN);
    } else {
        ui::showToast("Format Failed", "f_mkfs / mount error",
                      "Err code: " + std::to_string(fres), cfg::COL_RED);
    }
}
