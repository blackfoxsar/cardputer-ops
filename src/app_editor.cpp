#include "app_editor.h"

#include <SD.h>
#include <SPI.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "ui_common.h"

namespace {

constexpr size_t kMaxEditFileBytes = 32 * 1024;
constexpr int kEditorVisRows = 8;
constexpr int kEditorVisCols = 38;

struct SdEditorGuard {
    bool spi_started = false;
    bool mounted = false;

    bool mount() {
        SPI.begin(cfg::SD_SCK, cfg::SD_MISO, cfg::SD_MOSI, cfg::SD_CS);
        spi_started = true;
        mounted = SD.begin(cfg::SD_CS, SPI, 25000000);
        return mounted;
    }

    ~SdEditorGuard() {
        if (mounted) SD.end();
        if (spi_started) SPI.end();
    }
};

struct FsEntry {
    std::string name;
    std::string full_path;
    bool is_dir = false;
    bool is_parent = false;
    bool is_new_action = false;
    uint32_t size = 0;
};

std::string parentDir(const std::string& dir) {
    if (dir == "/" || dir.empty()) return "/";
    size_t slash = dir.rfind('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return dir.substr(0, slash);
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir == "/") return "/" + name;
    return dir + "/" + name;
}

std::string baseFilename(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

std::string formatKb(uint32_t bytes) {
    if (bytes >= 1024) {
        return std::to_string((bytes + 512) / 1024) + "K";
    }
    return std::to_string(bytes) + "B";
}

std::vector<FsEntry> listSdDir(const std::string& cur_dir) {
    std::vector<FsEntry> entries;

    FsEntry nf;
    nf.name = "[ + New File ]";
    nf.is_new_action = true;
    entries.push_back(nf);

    if (cur_dir != "/") {
        FsEntry up;
        up.name = "[ .. ]";
        up.full_path = parentDir(cur_dir);
        up.is_dir = true;
        up.is_parent = true;
        entries.push_back(up);
    }

    std::vector<FsEntry> dirs;
    std::vector<FsEntry> files;
    File dir = SD.open(cur_dir.c_str());
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            std::string raw = f.name();
            std::string base = baseFilename(raw);
            if (!base.empty() && base[0] != '.') {
                FsEntry e;
                e.name = base;
                e.full_path = joinPath(cur_dir, base);
                e.is_dir = f.isDirectory();
                e.size = static_cast<uint32_t>(f.size());
                if (e.is_dir) dirs.push_back(e);
                else files.push_back(e);
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
    }

    auto byName = [](const FsEntry& a, const FsEntry& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    entries.insert(entries.end(), files.begin(), files.end());
    return entries;
}

bool loadLinesFromSd(const std::string& path, std::vector<std::string>& lines, std::string& err) {
    lines.clear();
    if (!SD.exists(path.c_str())) {
        lines.push_back("");
        return true;
    }
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) {
        err = "Cannot open file";
        return false;
    }
    size_t sz = static_cast<size_t>(f.size());
    if (sz > kMaxEditFileBytes) {
        f.close();
        err = "File too large (>32 KB)";
        return false;
    }

    std::string raw;
    raw.reserve(sz);
    uint8_t chunk[256];
    while (f.available()) {
        int n = f.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            uint8_t b = chunk[i];
            if (b == '\r') continue;
            if (b == '\n') {
                raw.push_back('\n');
            } else if (b == '\t') {
                raw.append("  ");
            } else if (b >= 0x20 && b < 0x7F) {
                raw.push_back(static_cast<char>(b));
            }
        }
    }
    f.close();

    size_t pos = 0;
    while (pos <= raw.size()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(raw.substr(pos));
            break;
        }
        lines.push_back(raw.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (lines.empty()) lines.push_back("");
    return true;
}

bool saveLinesToSd(const std::string& path, const std::vector<std::string>& lines) {
    File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) return false;
    bool ok = true;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].empty()) {
            if (f.write(reinterpret_cast<const uint8_t*>(lines[i].data()), lines[i].size()) != lines[i].size()) {
                ok = false;
                break;
            }
        }
        if (i + 1 < lines.size()) {
            if (f.write(static_cast<uint8_t>('\n')) != 1) {
                ok = false;
                break;
            }
        }
    }
    f.flush();
    f.close();
    return ok;
}

// Return 0 = keep editing, 1 = discard/exit, 2 = saved & exit.
int promptSaveOnExit(const std::string& fname) {
    ui::waitKeyRelease();
    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Unsaved Changes");
        cv.setTextColor(cfg::COL_YELLOW, cfg::COL_BG);
        cv.drawString(ui::fitCols(fname, 36).c_str(), 10, 34);
        cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        cv.drawString("Save changes before closing?", 10, 54);
        ui::drawHints("Y save  N discard", "ESC back to edit");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0) {
            ui::waitKeyRelease();
            return 0;
        }
        if (lc == 'y') {
            ui::waitKeyRelease();
            return 2;
        }
        if (lc == 'n') {
            ui::waitKeyRelease();
            return 1;
        }
        delay(25);
    }
}

void editFileLoop(const std::string& path) {
    std::vector<std::string> lines;
    std::string err;
    if (!loadLinesFromSd(path, lines, err)) {
        ui::showToast("Open Error", path, err, cfg::COL_RED);
        return;
    }

    std::string fname = baseFilename(path);
    int cur_r = 0;
    int cur_c = 0;
    int row_scroll = 0;
    int col_scroll = 0;
    bool dirty = false;
    uint32_t saved_banner_until = 0;
    uint32_t last_blink = millis();
    bool blink = true;

    ui::waitKeyRelease();

    while (true) {
        if (millis() - last_blink >= 450) {
            blink = !blink;
            last_blink = millis();
        }

        if (cur_r < 0) cur_r = 0;
        if (cur_r >= static_cast<int>(lines.size())) cur_r = static_cast<int>(lines.size()) - 1;
        if (cur_c < 0) cur_c = 0;
        if (cur_c > static_cast<int>(lines[cur_r].size())) cur_c = static_cast<int>(lines[cur_r].size());

        if (cur_r < row_scroll) row_scroll = cur_r;
        if (cur_r >= row_scroll + kEditorVisRows) row_scroll = cur_r - kEditorVisRows + 1;
        if (cur_c < col_scroll) col_scroll = cur_c;
        if (cur_c >= col_scroll + kEditorVisCols) col_scroll = cur_c - kEditorVisCols + 1;

        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader(fname.c_str());

        char pos_buf[48];
        std::snprintf(pos_buf, sizeof(pos_buf), "Ln %d/%d  Col %d",
                      cur_r + 1, static_cast<int>(lines.size()), cur_c + 1);
        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString(pos_buf, 6, 23);

        cv.setTextDatum(top_right);
        if (millis() < saved_banner_until) {
            cv.setTextColor(cfg::COL_GREEN, cfg::COL_BG);
            cv.drawString("[SAVED]", 234, 23);
        } else if (dirty) {
            cv.setTextColor(cfg::COL_YELLOW, cfg::COL_BG);
            cv.drawString("[MOD]", 234, 23);
        } else {
            cv.setTextColor(cfg::COL_DIM, cfg::COL_BG);
            cv.drawString("Ctrl+S", 234, 23);
        }
        cv.setTextDatum(top_left);

        for (int i = 0; i < kEditorVisRows; ++i) {
            int r = row_scroll + i;
            if (r >= static_cast<int>(lines.size())) break;
            int y = 34 + i * 10;
            const std::string& line = lines[r];
            std::string vis = (col_scroll < static_cast<int>(line.size()))
                                  ? line.substr(col_scroll, kEditorVisCols)
                                  : "";
            cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            cv.drawString(vis.c_str(), 6, y);

            if (r == cur_r && blink) {
                int cx = 6 + (cur_c - col_scroll) * 6;
                cv.fillRect(cx, y, 2, 9, cfg::COL_ORANGE);
            }
        }

        ui::drawHints("Fn+arrows move  Ctrl+S save", "ESC / G0 close");
        cv.pushSprite(0, 0);

        // Poll without applyMenuNav so ; . , / type directly into the file!
        KeyInput k = ui::pollInput(90);
        if (k.esc || k.btn_g0) {
            if (!dirty) {
                ui::waitKeyRelease();
                return;
            }
            int choice = promptSaveOnExit(fname);
            if (choice == 1) return;
            if (choice == 2) {
                if (saveLinesToSd(path, lines)) {
                    return;
                }
                ui::showToast("Save Error", "Failed writing SD file", path, cfg::COL_RED);
                ui::waitKeyRelease();
            }
            continue;
        }

        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        bool reset_blink = false;

        if (k.ctrl && lc == 's') {
            if (saveLinesToSd(path, lines)) {
                dirty = false;
                saved_banner_until = millis() + 1500;
            } else {
                ui::showToast("Save Error", "Write failed:", path, cfg::COL_RED);
                ui::waitKeyRelease();
            }
        } else if (k.up) {
            if (cur_r > 0) --cur_r;
            reset_blink = true;
        } else if (k.down) {
            if (cur_r + 1 < static_cast<int>(lines.size())) ++cur_r;
            reset_blink = true;
        } else if (k.left) {
            if (cur_c > 0) {
                --cur_c;
            } else if (cur_r > 0) {
                --cur_r;
                cur_c = static_cast<int>(lines[cur_r].size());
            }
            reset_blink = true;
        } else if (k.right) {
            if (cur_c < static_cast<int>(lines[cur_r].size())) {
                ++cur_c;
            } else if (cur_r + 1 < static_cast<int>(lines.size())) {
                ++cur_r;
                cur_c = 0;
            }
            reset_blink = true;
        } else if (k.enter) {
            std::string tail = lines[cur_r].substr(cur_c);
            lines[cur_r].erase(cur_c);
            lines.insert(lines.begin() + cur_r + 1, tail);
            ++cur_r;
            cur_c = 0;
            dirty = true;
            reset_blink = true;
        } else if (k.del) {
            if (k.fn) {
                if (cur_c < static_cast<int>(lines[cur_r].size())) {
                    lines[cur_r].erase(cur_c, 1);
                    dirty = true;
                } else if (cur_r + 1 < static_cast<int>(lines.size())) {
                    lines[cur_r] += lines[cur_r + 1];
                    lines.erase(lines.begin() + cur_r + 1);
                    dirty = true;
                }
            } else if (cur_c > 0) {
                lines[cur_r].erase(cur_c - 1, 1);
                --cur_c;
                dirty = true;
            } else if (cur_r > 0) {
                int prev_len = static_cast<int>(lines[cur_r - 1].size());
                lines[cur_r - 1] += lines[cur_r];
                lines.erase(lines.begin() + cur_r);
                --cur_r;
                cur_c = prev_len;
                dirty = true;
            }
            reset_blink = true;
        } else if (k.tab) {
            lines[cur_r].insert(cur_c, "  ");
            cur_c += 2;
            dirty = true;
            reset_blink = true;
        } else if (!k.fn && !k.ctrl && !k.opt && k.ch >= 0x20 && k.ch < 0x7F) {
            lines[cur_r].insert(cur_c, 1, k.ch);
            ++cur_c;
            dirty = true;
            reset_blink = true;
        }

        if (reset_blink) {
            blink = true;
            last_blink = millis();
        }
        delay(15);
    }
}

}  // namespace

void runTextEditorApp() {
    ui::waitKeyRelease();
    SdEditorGuard sd;
    if (!sd.mount()) {
        ui::showToast("SD Error", "SD card not detected", "Insert FAT32 microSD card", cfg::COL_RED);
        return;
    }
    if (!SD.exists(cfg::SD_DIR_NOTES)) {
        SD.mkdir(cfg::SD_DIR_NOTES);
    }

    std::string cur_dir = cfg::SD_DIR_NOTES;
    auto list = listSdDir(cur_dir);
    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 5;

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        std::string hdr = "Edit: " + cur_dir;
        ui::drawHeader(hdr.c_str());

        for (int i = 0; i < kMaxVis && (scroll + i) < static_cast<int>(list.size()); ++i) {
            int idx = scroll + i;
            const auto& e = list[idx];
            int y = 26 + i * 18;
            bool sel = (idx == cursor);
            if (sel) {
                cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                uint16_t fg = e.is_new_action ? cfg::COL_GREEN
                                              : (e.is_dir ? cfg::COL_CYAN : cfg::COL_CREAM);
                cv.setTextColor(fg, cfg::COL_BG);
            }
            std::string label = (e.is_dir && !e.is_parent) ? (e.name + "/") : e.name;
            cv.drawString(ui::fitCols(label, 29).c_str(), 8, y + 2);
            if (!e.is_dir && !e.is_new_action) {
                if (!sel) cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
                cv.setTextDatum(top_right);
                cv.drawString(formatKb(e.size).c_str(), 232, y + 2);
                cv.setTextDatum(top_left);
            }
        }

        const auto& cur = list[cursor];
        const char* left_h = cur.is_new_action ? "Enter new file"
                                               : (cur.is_dir ? "Enter open dir" : "Enter edit  D del");
        ui::drawHints(left_h, "ESC / G0 back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0) {
            ui::waitKeyRelease();
            return;
        }
        if (k.up) {
            cursor = (cursor - 1 + static_cast<int>(list.size())) % static_cast<int>(list.size());
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, static_cast<int>(list.size()) - kMaxVis);
        } else if (k.down) {
            cursor = (cursor + 1) % static_cast<int>(list.size());
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (lc == 'd' && !cur.is_dir && !cur.is_new_action) {
            if (ui::promptConfirm("Delete File?", cur.name, cur.full_path)) {
                SD.remove(cur.full_path.c_str());
                list = listSdDir(cur_dir);
                if (cursor >= static_cast<int>(list.size())) cursor = static_cast<int>(list.size()) - 1;
                if (cursor < scroll) scroll = cursor;
            }
            ui::waitKeyRelease();
        } else if (k.enter) {
            if (cur.is_new_action) {
                std::string fname = "note.txt";
                if (ui::promptText("New Text File", "In " + cur_dir, fname, false, 40) && !fname.empty()) {
                    if (fname.find('/') != std::string::npos) {
                        ui::showToast("Invalid Name", "Filename cannot contain '/'", "", cfg::COL_RED);
                    } else {
                        std::string full = joinPath(cur_dir, fname);
                        editFileLoop(full);
                        list = listSdDir(cur_dir);
                    }
                }
                ui::waitKeyRelease();
            } else if (cur.is_dir) {
                cur_dir = cur.full_path;
                list = listSdDir(cur_dir);
                cursor = 0;
                scroll = 0;
            } else {
                editFileLoop(cur.full_path);
                list = listSdDir(cur_dir);
                ui::waitKeyRelease();
            }
        }
        delay(25);
    }
}
