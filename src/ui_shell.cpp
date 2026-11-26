#include "ui_shell.h"

#include <M5Cardputer.h>
#include <WiFi.h>
#include <driver/gpio.h>

#include <algorithm>
#include <cctype>
#include <vector>

#include "app_music.h"
#include "app_recorder.h"
#include "app_sd_format.h"
#include "app_snake.h"
#include "app_sudoku.h"
#include "app_time.h"
#include "app_wifi.h"
#include "config.h"
#include "csv_codec.h"
#include "session_logger.h"
#include "ssh_engine.h"
#include "storage.h"
#include "time_sync.h"
#include "ui_common.h"
#include "vt100_term.h"

namespace {

SshEngine g_ssh;
VT100Term g_term(cfg::TERM_COLS, cfg::TERM_ROWS);

// ── SSH Keys Screen ───────────────────────────────────────────────────

void runKeyGenDialog() {
    const char* types[] = {"Ed25519 (recommended)", "ECDSA P-256", "RSA 2048 (legacy compat)"};
    SshKeyKind kinds[] = {SshKeyKind::ED25519, SshKeyKind::ECDSA_P256, SshKeyKind::RSA_2048};
    const char* def_names[] = {"id_ed25519", "id_ecdsa", "id_rsa"};
    int sel = 0;
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Generate SSH Key");
        for (int i = 0; i < 3; ++i) {
            int y = 32 + i * 22;
            if (i == sel) {
                cv.fillRect(6, y - 2, 228, 18, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(types[i], 12, y + 2);
        }
        ui::drawHints("Enter choose", "Q / G0 cancel");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') return;
        if (k.up) sel = (sel + 2) % 3;
        if (k.down) sel = (sel + 1) % 3;
        if (k.enter) {
            std::string name = def_names[sel];
            if (!ui::promptText("Key Filename", "Saved in /keys/<name>", name, false, 32) || name.empty()) {
                return;
            }
            cv.fillScreen(cfg::COL_BG);
            ui::drawHeader("Generating Key...");
            cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            cv.drawString(name.c_str(), 12, 46);
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString("RSA-2048 can take 5-15s", 12, 66);
            ui::drawHints("Working...");
            cv.pushSprite(0, 0);

            KeyGenResult res = SshEngine::generateKeyPair(kinds[sel], name);
            if (res.ok) {
                ui::showToast("Key Generated", "/keys/" + res.priv_name, "View key -> E export SD", cfg::COL_GREEN);
            } else {
                ui::showToast("KeyGen Failed", res.error, "", cfg::COL_RED);
            }
            return;
        }
        delay(25);
    }
}

void runKeyExportDialog(const std::string& priv_name, bool has_pub) {
    KeySdResult res;
    if (!has_pub) {
        if (!ui::promptConfirm("Export Key to SD?", priv_name, "Save to /sd/ssh/keys/")) {
            return;
        }
        res = storage::exportKeyToSD(priv_name, true, false);
    } else {
        const char* opts[2] = {
            "1. Private + .pub (pair)",
            "2. Public key (.pub) only",
        };
        int sel = 0;
        bool chosen = false;
        bool export_priv = true;
        bool export_pub = true;
        ui::waitKeyRelease();

        while (!chosen) {
            auto& cv = ui::canvas();
            cv.fillScreen(cfg::COL_BG);
            ui::drawHeader("Export Key to SD");

            for (int i = 0; i < 2; ++i) {
                int y = 28 + i * 20;
                if (i == sel) {
                    cv.fillRect(6, y - 2, 228, 17, cfg::COL_ORANGE);
                    cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
                } else {
                    cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
                }
                cv.drawString(opts[i], 12, y + 2);
            }

            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString(ui::fitCols("/sd/ssh/keys/" + priv_name, 37).c_str(), 10, 76);
            ui::drawHints("Enter / 1-2 export", "Q cancel");
            cv.pushSprite(0, 0);

            KeyInput k = ui::pollInput();
            ui::applyMenuNav(k);
            char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
            if (k.esc || k.btn_g0 || lc == 'q') {
                ui::waitKeyRelease();
                return;
            }
            if (k.up || k.down) sel = 1 - sel;
            else if (k.ch == '1') {
                export_priv = true;
                export_pub = true;
                chosen = true;
            } else if (k.ch == '2') {
                export_priv = false;
                export_pub = true;
                chosen = true;
            } else if (k.enter) {
                export_priv = (sel == 0);
                export_pub = true;
                chosen = true;
            }
            delay(25);
        }
        res = storage::exportKeyToSD(priv_name, export_priv, export_pub);
    }

    if (!res.sd_ok) {
        ui::showToast("SD Error", res.error, "Insert FAT32 microSD card", cfg::COL_RED);
    } else if (res.ok) {
        const char* desc = (res.did_priv && res.did_pub)
                               ? "Private + .pub saved"
                               : (res.did_pub ? ".pub saved" : "Private key saved");
        ui::showToast("Exported to SD", "/sd/ssh/keys/" + priv_name, desc, cfg::COL_GREEN);
    } else {
        ui::showToast("Export Failed", res.error, "", cfg::COL_RED);
    }
}

void runKeyDetailScreen(const std::string& priv_name) {
    std::string priv;
    std::string pub;
    storage::readKeyFile(priv_name, priv);
    bool has_pub = storage::readKeyFile(priv_name + ".pub", pub);
    while (!pub.empty() && (pub.back() == '\n' || pub.back() == '\r')) {
        pub.pop_back();
    }

    std::vector<std::string> lines;
    if (has_pub && !pub.empty()) {
        for (size_t pos = 0; pos < pub.size(); pos += 37) {
            lines.push_back(pub.substr(pos, 37));
        }
    }

    constexpr size_t kMaxVis = 5;
    size_t scroll = 0;
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader(priv_name.c_str(), has_pub ? "PRIV+PUB" : "PRIV",
                       has_pub ? cfg::COL_GREEN : cfg::COL_YELLOW);

        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        std::string info = "Priv: " + std::to_string(priv.size()) + " B   " +
                           (has_pub ? ("Pub: " + std::to_string(pub.size()) + " B") : "Pub: none");
        cv.drawString(ui::fitCols(info, 37).c_str(), 8, 25);

        if (lines.empty()) {
            cv.setTextColor(cfg::COL_YELLOW, cfg::COL_BG);
            cv.drawString("No .pub sidecar found", 8, 44);
        } else {
            for (size_t i = 0; i < kMaxVis && (scroll + i) < lines.size(); ++i) {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
                cv.drawString(lines[scroll + i].c_str(), 8, 42 + static_cast<int>(i) * 14);
            }
        }

        bool scrollable = (lines.size() > kMaxVis);
        ui::drawHints("E export SD", scrollable ? ";. scroll  Q back" : "Q back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        if (lc == 'e') {
            runKeyExportDialog(priv_name, has_pub);
            ui::waitKeyRelease();
        } else if (k.up && scroll > 0) {
            --scroll;
        } else if (k.down && scrollable && (scroll + kMaxVis) < lines.size()) {
            ++scroll;
        }
        delay(25);
    }
}

void runKeyImportSdScreen() {
    bool sd_ok = false;
    std::string err;
    auto sd_keys = storage::listSdKeys(sd_ok, err);
    if (!sd_ok) {
        ui::showToast("SD Error", err, "Insert FAT32 microSD card", cfg::COL_RED);
        return;
    }
    if (sd_keys.empty()) {
        ui::showToast("No Keys on SD", "Put keys in /sd/ssh/keys/", "", cfg::COL_YELLOW);
        return;
    }

    std::vector<std::string> opts;
    bool has_all = (sd_keys.size() > 1);
    if (has_all) {
        opts.push_back("[ Import ALL (" + std::to_string(sd_keys.size()) + ") ]");
    }
    for (const auto& name : sd_keys) {
        opts.push_back(name);
    }

    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 5;
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Import Key (SD)");

        for (int i = 0; i < kMaxVis && (scroll + i) < static_cast<int>(opts.size()); ++i) {
            int idx = scroll + i;
            int y = 26 + i * 18;
            bool sel = (idx == cursor);
            if (sel) {
                cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor((has_all && idx == 0) ? cfg::COL_GREEN : cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(ui::fitCols(opts[idx], 34).c_str(), 10, y + 2);
        }

        ui::drawHints("Enter import", "Q back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        if (k.up) {
            cursor = (cursor - 1 + static_cast<int>(opts.size())) % static_cast<int>(opts.size());
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, static_cast<int>(opts.size()) - kMaxVis);
        } else if (k.down) {
            cursor = (cursor + 1) % static_cast<int>(opts.size());
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (k.enter) {
            if (has_all && cursor == 0) {
                int ok_cnt = 0;
                for (const auto& name : sd_keys) {
                    if (storage::importKeyFromSD(name).ok) {
                        ++ok_cnt;
                    }
                }
                ui::showToast("Import Complete",
                              "Imported " + std::to_string(ok_cnt) + "/" + std::to_string(sd_keys.size()) + " keys",
                              "/keys/", ok_cnt > 0 ? cfg::COL_GREEN : cfg::COL_RED);
                return;
            }
            const std::string& key_name = opts[cursor];
            auto existing = storage::listKeys(false);
            if (std::find(existing.begin(), existing.end(), key_name) != existing.end()) {
                if (!ui::promptConfirm("Overwrite Key?", key_name, "Replace existing in /keys/?")) {
                    ui::waitKeyRelease();
                    continue;
                }
            }
            auto res = storage::importKeyFromSD(key_name);
            if (!res.sd_ok) {
                ui::showToast("SD Error", res.error, "Insert FAT32 microSD card", cfg::COL_RED);
            } else if (res.ok) {
                ui::showToast("Key Imported", "/keys/" + key_name,
                              res.did_pub ? "Private + .pub imported" : "Private key imported",
                              cfg::COL_GREEN);
            } else {
                ui::showToast("Import Failed", res.error, "", cfg::COL_RED);
            }
            ui::waitKeyRelease();
        }
        delay(25);
    }
}

void runKeysScreen() {
    auto keys = storage::listKeys(false);
    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 5;
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("SSH Keys (/keys)");
        if (keys.empty()) {
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString("No private keys stored", 44, 48);
            cv.drawString("G gen  or  I import from SD", 34, 66);
        } else {
            for (int i = 0; i < kMaxVis && (scroll + i) < static_cast<int>(keys.size()); ++i) {
                int idx = scroll + i;
                int y = 26 + i * 18;
                bool sel = (idx == cursor);
                if (sel) {
                    cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                    cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
                } else {
                    cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
                }
                cv.drawString(ui::fitCols(keys[idx], 34).c_str(), 10, y + 2);
            }
        }
        ui::drawHints("G:Gen I:SD Enter", "D:Del Q:Back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') return;
        if (lc == 'g') {
            runKeyGenDialog();
            keys = storage::listKeys(false);
            cursor = scroll = 0;
            ui::waitKeyRelease();
        } else if (lc == 'i') {
            runKeyImportSdScreen();
            keys = storage::listKeys(false);
            if (cursor >= static_cast<int>(keys.size())) {
                cursor = keys.empty() ? 0 : static_cast<int>(keys.size()) - 1;
            }
            if (cursor < scroll) scroll = cursor;
            ui::waitKeyRelease();
        } else if (k.up && !keys.empty()) {
            cursor = (cursor - 1 + static_cast<int>(keys.size())) % static_cast<int>(keys.size());
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, static_cast<int>(keys.size()) - kMaxVis);
        } else if (k.down && !keys.empty()) {
            cursor = (cursor + 1) % static_cast<int>(keys.size());
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (lc == 'd' && !keys.empty()) {
            if (ui::promptConfirm("Delete Key?", keys[cursor], "Deletes private & .pub")) {
                storage::deleteKeyPair(keys[cursor]);
                keys = storage::listKeys(false);
                if (cursor >= static_cast<int>(keys.size()) && cursor > 0) --cursor;
                if (cursor < scroll) scroll = cursor;
            }
            ui::waitKeyRelease();
        } else if (k.enter && !keys.empty()) {
            runKeyDetailScreen(keys[cursor]);
            ui::waitKeyRelease();
        }
        delay(25);
    }
}

// ── Host Editor & Picker ──────────────────────────────────────────────

std::string pickKeyDialog(const std::string& current) {
    auto keys = storage::listKeys(false);
    std::vector<std::string> opts;
    opts.push_back("(none - password auth)");
    for (const auto& k : keys) opts.push_back(k);

    int sel = 0;
    for (size_t i = 1; i < opts.size(); ++i) {
        if (opts[i] == current) {
            sel = static_cast<int>(i);
            break;
        }
    }

    int scroll = 0;
    constexpr int kMaxVis = 5;
    ui::waitKeyRelease();
    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Select SSH Key");
        for (int i = 0; i < kMaxVis && (scroll + i) < static_cast<int>(opts.size()); ++i) {
            int idx = scroll + i;
            int y = 26 + i * 18;
            if (idx == sel) {
                cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(ui::fitCols(opts[idx], 34).c_str(), 10, y + 2);
        }
        ui::drawHints("Enter select", "Q cancel");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') return current;
        if (k.up) {
            sel = (sel - 1 + static_cast<int>(opts.size())) % static_cast<int>(opts.size());
            if (sel < scroll) scroll = sel;
        } else if (k.down) {
            sel = (sel + 1) % static_cast<int>(opts.size());
            if (sel >= scroll + kMaxVis) scroll = sel - kMaxVis + 1;
        } else if (k.enter) {
            return sel == 0 ? "" : opts[sel];
        }
        delay(25);
    }
}

bool editHostForm(HostProfile& p, bool is_new) {
    if (is_new) {
        p.legacy = storage::loadDefaultLegacy();
    }
    int cursor = 0;
    int scroll = 0;
    constexpr int kFields = 8;
    constexpr int kMaxVis = 5;
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader(is_new ? "New SSH Host" : "Edit SSH Host");

        const char* labels[kFields] = {
            "Host", "User", "Password", "Port", "SSH Key", "Legacy SSH", "Alias", "[ Save Host ]"
        };
        std::string vals[kFields] = {
            p.host,
            p.user,
            p.password.empty() ? "(prompt)" : std::string(std::min<size_t>(p.password.size(), 10), '*'),
            std::to_string(p.port),
            p.key_file.empty() ? "(none)" : p.key_file,
            p.legacy ? "[X] Allow ssh-rsa/CBC" : "[ ] Modern only",
            p.name,
            ""
        };

        for (int i = 0; i < kMaxVis && (scroll + i) < kFields; ++i) {
            int idx = scroll + i;
            int y = 26 + i * 18;
            bool sel = (idx == cursor);
            if (sel) {
                cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(idx == 7 ? cfg::COL_GREEN : cfg::COL_CREAM, cfg::COL_BG);
            }
            std::string row = std::string(labels[idx]);
            if (!vals[idx].empty()) {
                row += ": " + vals[idx];
            }
            cv.drawString(ui::fitCols(row, 37).c_str(), 8, y + 2);
        }

        ui::drawHints("Enter edit/toggle", "S save  Q cancel");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0 || lc == 'q') return false;
        if (k.up) {
            cursor = (cursor - 1 + kFields) % kFields;
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = kFields - kMaxVis;
        } else if (k.down) {
            cursor = (cursor + 1) % kFields;
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (lc == 's' || (k.enter && cursor == 7)) {
            if (p.host.empty() || p.user.empty()) {
                ui::showToast("Missing Fields", "Host and User are required", "", cfg::COL_RED);
                continue;
            }
            return true;
        } else if (k.enter || (k.ch == ' ' && cursor == 5)) {
            if (cursor == 0) ui::promptText("Host / IP", "e.g. 192.168.1.10", p.host, false, 63);
            else if (cursor == 1) ui::promptText("Username", "e.g. root", p.user, false, 32);
            else if (cursor == 2) ui::promptText("Password", "Leave empty to prompt on connect", p.password, true, 63);
            else if (cursor == 3) {
                std::string ps = std::to_string(p.port);
                if (ui::promptText("SSH Port", "1 - 65535", ps, false, 5)) {
                    int v = std::atoi(ps.c_str());
                    if (v >= 1 && v <= 65535) p.port = static_cast<uint16_t>(v);
                }
            } else if (cursor == 4) {
                p.key_file = pickKeyDialog(p.key_file);
            } else if (cursor == 5) {
                p.legacy = !p.legacy;
            } else if (cursor == 6) {
                ui::promptText("Alias (optional)", "Display name in host list", p.name, false, 32);
            }
        }
        delay(25);
    }
}

// ── Active SSH Terminal Session ───────────────────────────────────────

uint16_t termPaletteColor(uint8_t idx, bool bold) {
    static constexpr uint16_t kAnsi16[16] = {
        0x0000, 0xA800, 0x0540, 0xAA80, 0x0015, 0xA815, 0x0555, 0xEF7D,
        0x52AA, 0xF800, 0x07E0, 0xFFE0, 0x54DF, 0xF81F, 0x07FF, 0xFFFF,
    };
    uint8_t c = idx & 0x0F;
    if (bold && c < 8) c += 8;
    return kAnsi16[c];
}

void renderTerminal(const HostProfile& profile, const char* log_tag = nullptr,
                    uint16_t log_col = cfg::COL_GREEN) {
    auto& cv = ui::canvas();
    cv.fillRect(0, 0, cfg::SCREEN_W, cfg::TERM_BAR_H, cfg::COL_DARK);
    cv.fillRect(0, cfg::TERM_BAR_H - 1, cfg::SCREEN_W, 1, cfg::COL_ORANGE);
    cv.setTextSize(1);
    cv.setTextColor(cfg::COL_ORANGE, cfg::COL_DARK);
    size_t max_hdr_cols = (log_tag && *log_tag) ? 16 : 21;
    std::string hdr = ui::fitCols(profile.displayLabel(), max_hdr_cols);
    if (profile.legacy) hdr += " [L]";
    cv.drawString(hdr.c_str(), 4, 3);

    int pct = ui::readBatteryPct();
    std::string bat = ui::batteryText(pct);
    cv.setTextDatum(top_right);
    cv.setTextColor(cfg::COL_GRAY, cfg::COL_DARK);
    cv.drawString("G0:Menu", cfg::SCREEN_W - 4, 3);
    cv.setTextColor(ui::batteryColor(pct), cfg::COL_DARK);
    cv.drawString(bat.c_str(), cfg::SCREEN_W - 52, 3);
    if (log_tag && *log_tag) {
        cv.setTextColor(log_col, cfg::COL_DARK);
        cv.drawString(log_tag, cfg::SCREEN_W - 84, 3);
    }
    cv.setTextDatum(top_left);

    for (int r = 0; r < g_term.rows(); ++r) {
        int y = cfg::TERM_BAR_H + r * 8;
        for (int c = 0; c < g_term.cols(); ++c) {
            const auto& cell = g_term.cell(r, c);
            bool is_cur = g_term.cursorVisible() && (r == g_term.cursorRow() && c == g_term.cursorCol());
            uint16_t fg = termPaletteColor(cell.fg, cell.bold);
            uint16_t bg = termPaletteColor(cell.bg, false);
            if (cell.inverse || is_cur) {
                std::swap(fg, bg);
                if (is_cur && fg == bg) {
                    bg = cfg::COL_ORANGE;
                    fg = cfg::COL_BG;
                }
            }
            int x = c * 6;
            cv.fillRect(x, y, 6, 8, bg);
            if (cell.ch != ' ') {
                cv.setTextColor(fg, bg);
                char buf[2] = {cell.ch, 0};
                cv.drawString(buf, x, y);
            }
        }
    }
    g_term.clearDirty();
    cv.pushSprite(0, 0);
}

int runSessionOverlay() {
    auto& cv = ui::canvas();
    const char* items[] = {"Resume Terminal", "Send Ctrl-C (^C)", "Send ESC", "Disconnect"};
    int sel = 0;
    ui::waitKeyRelease();
    while (true) {
        cv.fillRect(30, 24, 180, 90, cfg::COL_DARK);
        cv.drawRect(30, 24, 180, 90, cfg::COL_ORANGE);
        for (int i = 0; i < 4; ++i) {
            int y = 32 + i * 19;
            if (i == sel) {
                cv.fillRect(36, y - 2, 168, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(i == 3 ? cfg::COL_RED : cfg::COL_CREAM, cfg::COL_DARK);
            }
            cv.drawString(items[i], 44, y + 2);
        }
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        if (k.esc || k.btn_g0) return 0;
        if (k.up) sel = (sel + 3) % 4;
        if (k.down) sel = (sel + 1) % 4;
        if (k.enter) return sel;
        delay(25);
    }
}

void runSshSession(const HostProfile& profile) {
    if (WiFi.status() != WL_CONNECTED) {
        ui::showToast("WiFi Offline", "Open Wi-Fi to connect", "", cfg::COL_RED);
        return;
    }

    ui::waitKeyRelease();
    g_term.reset();
    if (!g_ssh.startConnect(profile)) {
        ui::showToast("SSH Error", g_ssh.errorText(), "", cfg::COL_RED);
        return;
    }

    uint8_t rx_buf[512];
    int last_bat_pct = ui::readBatteryPct();
    SessionLogger logger;
    bool want_log = storage::loadSettings().sd_log_enabled;
    bool log_started = false;

    while (true) {
        auto st = g_ssh.state();

        if (st == SshEngine::State::CONNECTING) {
            auto& cv = ui::canvas();
            cv.fillScreen(cfg::COL_BG);
            ui::drawHeader("SSH Connecting", profile.legacy ? "LEGACY" : "MODERN",
                           profile.legacy ? cfg::COL_YELLOW : cfg::COL_GREEN);
            cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            cv.drawString(ui::fitCols(profile.displayLabel(), 36).c_str(), 10, 42);
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString(ui::fitCols(g_ssh.statusText(), 36).c_str(), 10, 62);
            ui::drawHints("G0 / ESC cancel");
            cv.pushSprite(0, 0);

            KeyInput k = ui::pollInput();
            if (k.esc || k.btn_g0) {
                g_ssh.disconnect();
                logger.stop();
                return;
            }
            delay(30);
            continue;
        }

        if (st == SshEngine::State::PROMPT_HOST_KEY) {
            auto info = g_ssh.hostPromptInfo();
            std::string l1 = (info.changed ? "KEY CHANGED: " : "New ") + info.key_type;
            std::string l2 = info.fingerprint;
            std::string l3 = l2.size() > 35 ? l2.substr(35) : "";
            bool ok = ui::promptConfirm(info.changed ? "WARNING: Host Changed" : "Trust Host Key?", l1, l2, l3);
            g_ssh.answerHostPrompt(ok);
            if (!ok) {
                g_ssh.disconnect();
                logger.stop();
                return;
            }
            continue;
        }

        if (st == SshEngine::State::PROMPT_INPUT) {
            auto info = g_ssh.inputPromptInfo();
            std::string val;
            bool ok = ui::promptText(info.title.c_str(), info.subtitle, val, info.masked, 63);
            g_ssh.answerInputPrompt(ok, val);
            if (!ok) {
                g_ssh.disconnect();
                logger.stop();
                return;
            }
            continue;
        }

        if (st == SshEngine::State::ERROR || st == SshEngine::State::DISCONNECTED) {
            std::string saved_log = logger.active() ? logger.path() : "";
            logger.stop();
            std::string err = g_ssh.errorText();
            ui::showToast(st == SshEngine::State::ERROR ? "SSH Error" : "Disconnected",
                          err.empty() ? g_ssh.statusText() : err,
                          saved_log.empty() ? "" : ("Log: " + saved_log),
                          st == SshEngine::State::ERROR ? cfg::COL_RED : cfg::COL_YELLOW);
            g_ssh.disconnect();
            return;
        }

        bool need_redraw = false;
        if (!log_started) {
            log_started = true;
            if (want_log) {
                logger.start(profile);
            }
            need_redraw = true;
        }

        int cur_bat_pct = ui::readBatteryPct();
        if (cur_bat_pct != last_bat_pct) {
            last_bat_pct = cur_bat_pct;
            need_redraw = true;
        }
        size_t rn = g_ssh.readRx(rx_buf, sizeof(rx_buf));
        if (rn > 0) {
            g_term.feed(reinterpret_cast<const char*>(rx_buf), rn);
            if (logger.active()) {
                logger.appendRaw(rx_buf, rn);
            }
            need_redraw = true;
        }
        logger.pollFlush();

        KeyInput k = ui::pollInput();
        if (k.btn_g0 || (k.fn && k.ch && std::tolower(static_cast<unsigned char>(k.ch)) == 'q')) {
            int action = runSessionOverlay();
            if (action == 1) {
                const uint8_t ctrl_c = 0x03;
                g_ssh.writeTx(&ctrl_c, 1);
            } else if (action == 2) {
                const uint8_t esc = 0x1B;
                g_ssh.writeTx(&esc, 1);
            } else if (action == 3) {
                std::string saved_log = logger.active() ? logger.path() : "";
                logger.stop();
                g_ssh.disconnect();
                if (!saved_log.empty()) {
                    ui::showToast("Log Saved", saved_log, "", cfg::COL_GREEN);
                }
                return;
            }
            need_redraw = true;
        } else {
            uint8_t seq[4];
            size_t slen = 0;
            if (k.up) { seq[0] = 0x1B; seq[1] = '['; seq[2] = 'A'; slen = 3; }
            else if (k.down) { seq[0] = 0x1B; seq[1] = '['; seq[2] = 'B'; slen = 3; }
            else if (k.right) { seq[0] = 0x1B; seq[1] = '['; seq[2] = 'C'; slen = 3; }
            else if (k.left) { seq[0] = 0x1B; seq[1] = '['; seq[2] = 'D'; slen = 3; }
            else if (k.enter) { seq[0] = '\r'; slen = 1; }
            else if (k.del) { seq[0] = 0x7F; slen = 1; }
            else if (k.tab) { seq[0] = '\t'; slen = 1; }
            else if (k.esc) { seq[0] = 0x1B; slen = 1; }
            else if ((k.ctrl || k.opt) && k.ch != 0) {
                char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch)));
                if (lc >= 'a' && lc <= 'z') {
                    seq[0] = static_cast<uint8_t>(lc - 'a' + 1);
                    slen = 1;
                } else if (lc == '[') { seq[0] = 0x1B; slen = 1; }
                else if (lc == '\\') { seq[0] = 0x1C; slen = 1; }
                else if (lc == ']') { seq[0] = 0x1D; slen = 1; }
            } else if (k.ch != 0) {
                seq[0] = static_cast<uint8_t>(k.ch);
                slen = 1;
            }
            if (slen > 0) {
                g_ssh.writeTx(seq, slen);
            }
        }

        if (need_redraw) {
            const char* log_tag = logger.active() ? "LOG" : ((want_log && logger.failed()) ? "NOSD" : nullptr);
            uint16_t log_col = logger.active() ? cfg::COL_GREEN : cfg::COL_RED;
            renderTerminal(profile, log_tag, log_col);
        }
        delay(12);
    }
}

// ── Hosts, SD Sync, Settings & SSH Submenu ────────────────────────────

void runHostsScreen() {
    auto hosts = storage::loadHosts();
    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 5;
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("SSH Hosts");

        if (hosts.empty()) {
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString("No saved hosts", 72, 48);
            cv.drawString("Press N to add or SD Import", 34, 66);
        } else {
            for (int i = 0; i < kMaxVis && (scroll + i) < static_cast<int>(hosts.size()); ++i) {
                int idx = scroll + i;
                int y = 26 + i * 18;
                bool sel = (idx == cursor);
                if (sel) {
                    cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                    cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
                } else {
                    cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
                }
                cv.drawString(ui::fitCols(hosts[idx].displayLabel(), 26).c_str(), 8, y + 2);

                std::string badges;
                if (hosts[idx].legacy) badges += "[L]";
                if (!hosts[idx].key_file.empty()) badges += "[K]";
                else if (!hosts[idx].password.empty()) badges += "[P]";
                cv.setTextDatum(top_right);
                cv.drawString(badges.c_str(), 232, y + 2);
                cv.setTextDatum(top_left);
            }
        }
        ui::drawHints("Enter:Go N:New E:Edit", "D:Del Q:Back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        if (k.up && !hosts.empty()) {
            cursor = (cursor - 1 + static_cast<int>(hosts.size())) % static_cast<int>(hosts.size());
            if (cursor < scroll) scroll = cursor;
        } else if (k.down && !hosts.empty()) {
            cursor = (cursor + 1) % static_cast<int>(hosts.size());
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
        } else if (lc == 'n') {
            HostProfile np;
            if (editHostForm(np, true)) {
                hosts.push_back(np);
                storage::saveHosts(hosts);
                cursor = static_cast<int>(hosts.size()) - 1;
                if (cursor >= kMaxVis) scroll = cursor - kMaxVis + 1;
            }
            ui::waitKeyRelease();
        } else if (lc == 'e' && !hosts.empty()) {
            HostProfile copy = hosts[cursor];
            if (editHostForm(copy, false)) {
                hosts[cursor] = copy;
                storage::saveHosts(hosts);
            }
            ui::waitKeyRelease();
        } else if (lc == 'd' && !hosts.empty()) {
            if (ui::promptConfirm("Delete Host?", hosts[cursor].displayLabel(), "Remove from /hosts.csv?")) {
                hosts.erase(hosts.begin() + cursor);
                storage::saveHosts(hosts);
                if (cursor >= static_cast<int>(hosts.size()) && cursor > 0) --cursor;
            }
            ui::waitKeyRelease();
        } else if (k.enter && !hosts.empty()) {
            runSshSession(hosts[cursor]);
            ui::waitKeyRelease();
        }
        delay(25);
    }
}

void runSdSyncScreen() {
    int sel = 0;
    ui::waitKeyRelease();
    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("SD Import / Export");

        const char* opts[2] = {
            "Import from /sd/ssh/ -> Flash",
            "Export Flash -> /sd/ssh/"
        };
        for (int i = 0; i < 2; ++i) {
            int y = 30 + i * 22;
            if (i == sel) {
                cv.fillRect(6, y - 2, 228, 18, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(opts[i], 12, y + 2);
        }
        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString("CSV: hosts.csv, wifi.csv, keys/*", 12, 82);
        ui::drawHints("Enter run", "Q back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') return;
        if (k.up || k.down) sel = 1 - sel;
        if (k.enter) {
            SyncReport rep = (sel == 0) ? storage::importFromSD() : storage::exportToSD();
            if (!rep.sd_ok) {
                ui::showToast("SD Error", rep.error, "Insert FAT32 microSD card", cfg::COL_RED);
            } else {
                std::string summary = "Hosts: " + std::to_string(rep.hosts_count) +
                                      "  WiFi: " + std::to_string(rep.wifi_count) +
                                      "  Keys: " + std::to_string(rep.keys_count);
                ui::showToast(sel == 0 ? "Import Complete" : "Export Complete",
                              summary, "/sd/ssh/", cfg::COL_GREEN);
            }
        }
        delay(25);
    }
}

void runSettingsScreen() {
    int sel = 0;
    constexpr int kRows = 3;
    AppSettings st = storage::loadSettings();
    ui::waitKeyRelease();
    while (true) {
        FsStats fs = storage::getFsStats();
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("SSH Settings");

        std::string row0 = std::string("SD Session Log: ") + (st.sd_log_enabled ? "ON" : "OFF");
        std::string row1 = std::string("Default Legacy SSH: ") + (st.default_legacy ? "ON" : "OFF");
        const char* row2 = "Clear Known Hosts Cache";
        const char* rows[kRows] = {row0.c_str(), row1.c_str(), row2};

        for (int i = 0; i < kRows; ++i) {
            int y = 26 + i * 19;
            if (i == sel) {
                cv.fillRect(6, y - 2, 228, 17, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(ui::fitCols(rows[i], 36).c_str(), 10, y + 2);
        }

        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        std::string heap_s = "Heap free: " + std::to_string(ESP.getFreeHeap() / 1024) + " KB";
        std::string fs_s = "LittleFS: " + std::to_string(fs.used_bytes / 1024) + "/" +
                           std::to_string(fs.total_bytes / 1024) + " KB";
        cv.drawString(heap_s.c_str(), 10, 86);
        cv.drawString(fs_s.c_str(), 10, 101);
        ui::drawHints("Enter toggle/run", "Q back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        if (k.up) {
            sel = (sel - 1 + kRows) % kRows;
        } else if (k.down) {
            sel = (sel + 1) % kRows;
        } else if (k.enter) {
            if (sel == 0) {
                st.sd_log_enabled = !st.sd_log_enabled;
                storage::saveSettings(st);
            } else if (sel == 1) {
                st.default_legacy = !st.default_legacy;
                storage::saveSettings(st);
            } else if (sel == 2 && ui::promptConfirm("Clear Known Hosts?", "Deletes /known_hosts.csv", "")) {
                storage::clearKnownHosts();
                ui::showToast("Cleared", "Known hosts reset", "", cfg::COL_GREEN);
                ui::waitKeyRelease();
            }
        }
        delay(25);
    }
}

void runGamesFolderApp() {
    int cursor = 0;
    const char* items[2] = {
        "1. Snake",
        "2. Sudoku",
    };

    ui::waitKeyRelease();
    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Games");

        for (int i = 0; i < 2; ++i) {
            int y = 30 + i * 22;
            if (i == cursor) {
                cv.fillRect(6, y - 2, 228, 18, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(items[i], 12, y + 2);
        }
        ui::drawHints("Enter / 1-2 play", "Q / G0 back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }

        int open_idx = -1;
        if (k.up || k.down) cursor = 1 - cursor;
        else if (k.enter) open_idx = cursor;
        else if (k.ch >= '1' && k.ch <= '2') {
            cursor = k.ch - '1';
            open_idx = cursor;
        }

        if (open_idx == 0) {
            runSnakeApp();
            ui::waitKeyRelease();
        } else if (open_idx == 1) {
            runSudokuApp();
            ui::waitKeyRelease();
        }
        delay(25);
    }
}

void runSshClientApp() {
    int cursor = 0;
    const char* items[4] = {
        "1. Hosts (Quick Connect)",
        "2. SSH Keys (Gen / View)",
        "3. SD Import / Export (CSV)",
        "4. SSH Settings & Info",
    };

    ui::waitKeyRelease();
    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("SSH Client");

        for (int i = 0; i < 4; ++i) {
            int y = 28 + i * 20;
            if (i == cursor) {
                cv.fillRect(4, y - 2, 232, 17, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(items[i], 10, y + 2);
        }
        ui::drawHints("Enter / 1-4 open", "Q back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }

        int open_idx = -1;
        if (k.up) cursor = (cursor + 3) % 4;
        else if (k.down) cursor = (cursor + 1) % 4;
        else if (k.enter) open_idx = cursor;
        else if (k.ch >= '1' && k.ch <= '4') {
            cursor = k.ch - '1';
            open_idx = cursor;
        }

        if (open_idx == 0) {
            runHostsScreen();
            ui::waitKeyRelease();
        } else if (open_idx == 1) {
            runKeysScreen();
            ui::waitKeyRelease();
        } else if (open_idx == 2) {
            runSdSyncScreen();
            ui::waitKeyRelease();
        } else if (open_idx == 3) {
            runSettingsScreen();
            ui::waitKeyRelease();
        }

        delay(25);
    }
}

}  // namespace

void UiShell::begin() {
    auto m5cfg = M5.config();
    M5Cardputer.begin(m5cfg, true);
    // M5Unified drives GPIO46 output-HIGH during board detection and does not restore it
    // on Cardputer-Adv. GPIO46 is the ES8311 ADC data line (ASDOUT); leaving it driven
    // makes the microphone record silence, so release it back to its reset state.
    gpio_reset_pin(GPIO_NUM_46);
    // M5GFX >= 0.3.0 already sets rotation_offset=1 for Cardputer-Adv (rotation 0 is 240x135).
    // Only apply setRotation(1) if the panel initialized in portrait orientation.
    if (M5Cardputer.Display.width() < M5Cardputer.Display.height()) {
        M5Cardputer.Display.setRotation(1);
    }

    ui::initCanvas();
    storage::begin();
    time_sync::applyTimezone(storage::loadSettings().tz_offset_hours);

    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_OFF);
}

void UiShell::tick() {
    static int cursor = 0;
    constexpr int kMenuItems = 7;
    const char* items[kMenuItems] = {
        "1.SSH Client",
        "2.Wi-Fi",
        "3.Date & Time",
        "4.SD Format",
        "5.Voice Rec",
        "6.Audio Playback",
        "7.Games >",
    };

    auto& cv = ui::canvas();
    cv.fillScreen(cfg::COL_BG);
    ui::drawHeader("Cardputer Ops");

    for (int i = 0; i < kMenuItems; ++i) {
        int col = i % 2;
        int row = i / 2;
        int x = (col == 0) ? 6 : 123;
        int y = 26 + row * 22;
        bool sel = (i == cursor);
        if (sel) {
            cv.fillRect(x, y, 111, 18, cfg::COL_ORANGE);
            cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
        } else {
            cv.fillRect(x, y, 111, 18, cfg::COL_DARK);
            cv.drawRect(x, y, 111, 18, cfg::COL_DIM);
            cv.setTextColor(cfg::COL_CREAM, cfg::COL_DARK);
        }
        cv.drawString(items[i], x + 6, y + 5);
    }
    ui::drawHints(";.,/ move  Enter open", "1-7 shortcut");
    cv.pushSprite(0, 0);

    KeyInput k = ui::pollInput();
    ui::applyMenuNav(k);

    int open_idx = -1;
    if (k.up) {
        if (cursor >= 2) cursor -= 2;
        else cursor = (cursor == 0) ? 6 : 5;
    } else if (k.down) {
        if (cursor + 2 < kMenuItems) cursor += 2;
        else cursor = (cursor % 2 == 0) ? 0 : 1;
    } else if (k.left || k.right) {
        int col = cursor % 2;
        if (k.left && col == 1) --cursor;
        else if (k.right && col == 0 && cursor + 1 < kMenuItems) ++cursor;
    } else if (k.enter) {
        open_idx = cursor;
    } else if (k.ch >= '1' && k.ch <= '7') {
        cursor = k.ch - '1';
        open_idx = cursor;
    }

    if (open_idx == 0) {
        runSshClientApp();
        ui::waitKeyRelease();
    } else if (open_idx == 1) {
        runWifiScannerApp();
        ui::waitKeyRelease();
    } else if (open_idx == 2) {
        runDateTimeApp();
        ui::waitKeyRelease();
    } else if (open_idx == 3) {
        runSdFormatApp();
        ui::waitKeyRelease();
    } else if (open_idx == 4) {
        runRecorderApp();
        ui::waitKeyRelease();
    } else if (open_idx == 5) {
        runMusicApp();
        ui::waitKeyRelease();
    } else if (open_idx == 6) {
        runGamesFolderApp();
        ui::waitKeyRelease();
    }

    delay(25);
}
