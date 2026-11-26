#include "ui_common.h"

#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "config.h"
#include "time_sync.h"

namespace {

std::vector<std::string> wrapLines(const std::string& text, size_t max_cols) {
    std::vector<std::string> out;
    if (max_cols == 0) return out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string para = text.substr(start, end - start);
        if (!para.empty() && para.back() == '\r') {
            para.pop_back();
        }
        if (para.empty()) {
            out.push_back("");
        } else {
            size_t pos = 0;
            while (pos < para.size()) {
                if (para.size() - pos <= max_cols) {
                    out.push_back(para.substr(pos));
                    break;
                }
                size_t split = pos + max_cols;
                size_t space = para.rfind(' ', split);
                if (space != std::string::npos && space >= pos + max_cols / 2) {
                    out.push_back(para.substr(pos, space - pos));
                    pos = space + 1;
                } else {
                    out.push_back(para.substr(pos, max_cols));
                    pos += max_cols;
                }
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

M5Canvas g_canvas(&M5Cardputer.Display);
uint32_t g_last_repeat_ms = 0;
std::string g_last_sig;
bool g_repeating = false;

std::string buildKeySig(const Keyboard_Class::KeysState& st) {
    std::string sig;
    if (st.enter) sig += 'E';
    if (st.del) sig += 'B';
    if (st.tab) sig += 'T';
    if (st.fn) sig += 'F';
    if (st.ctrl) sig += 'C';
    if (st.opt) sig += 'O';
    for (char c : st.word) sig.push_back(c);
    return sig;
}

bool hasActiveKey(const Keyboard_Class::KeysState& st) {
    return st.enter || st.del || st.tab || !st.word.empty() || M5Cardputer.BtnA.isPressed();
}

}  // namespace

namespace ui {

void initCanvas() {
    g_canvas.createSprite(cfg::SCREEN_W, cfg::SCREEN_H);
    g_canvas.setTextFont(0);
    g_canvas.setTextSize(1);
}

M5Canvas& canvas() {
    return g_canvas;
}

void waitKeyRelease(uint32_t max_ms) {
    uint32_t t0 = millis();
    M5Cardputer.update();
    while (hasActiveKey(M5Cardputer.Keyboard.keysState()) && (millis() - t0 < max_ms)) {
        delay(15);
        M5Cardputer.update();
    }
    // Drain any latched BtnA edge once keys are released
    delay(10);
    M5Cardputer.update();
    g_last_sig = buildKeySig(M5Cardputer.Keyboard.keysState());
    g_last_repeat_ms = millis();
    g_repeating = false;
}

KeyInput pollInput(uint32_t repeat_ms) {
    M5Cardputer.update();
    KeyInput out;

    if (M5Cardputer.BtnA.wasPressed()) {
        out.btn_g0 = true;
    }

    auto& kb = M5Cardputer.Keyboard;
    auto st = kb.keysState();

    std::string sig = buildKeySig(st);
    if (sig.empty()) {
        g_last_sig.clear();
        g_repeating = false;
        return out;
    }

    bool is_esc = (st.fn && !st.word.empty() && (st.word[0] == '`' || st.word[0] == '~'));
    bool non_repeat = st.enter || st.tab || is_esc;

    uint32_t now = millis();
    if (sig == g_last_sig) {
        if (non_repeat) {
            return out;
        }
        uint32_t need_ms = (!g_repeating && repeat_ms >= 150) ? std::max<uint32_t>(repeat_ms, 350) : repeat_ms;
        if (now - g_last_repeat_ms < need_ms) {
            return out;
        }
        g_repeating = true;
        g_last_repeat_ms = now;
    } else {
        g_last_sig = sig;
        g_last_repeat_ms = now;
        g_repeating = false;
    }

    out.fn = st.fn;
    out.ctrl = st.ctrl;
    out.opt = st.opt;
    out.enter = st.enter;
    out.del = st.del;
    out.tab = st.tab;

    if (!st.word.empty()) {
        char c = st.word[0];
        if (st.fn) {
            char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lc == ';' || c == ':') out.up = true;
            else if (lc == '.' || c == '>') out.down = true;
            else if (lc == ',' || c == '<') out.left = true;
            else if (lc == '/' || c == '?') out.right = true;
            else if (c == '`' || c == '~') out.esc = true;
            else out.ch = c;
        } else {
            out.ch = c;
        }
    }
    return out;
}

void applyMenuNav(KeyInput& in) {
    if (!in.ctrl && !in.opt && !in.fn && in.ch != 0) {
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(in.ch)));
        if (lc == ';') in.up = true;
        else if (lc == '.') in.down = true;
        else if (lc == ',') in.left = true;
        else if (lc == '/') in.right = true;
    }
}

std::string fitCols(const std::string& s, size_t max_chars) {
    if (s.size() <= max_chars) return s;
    if (max_chars <= 2) return s.substr(0, max_chars);
    return s.substr(0, max_chars - 2) + "..";
}

int readBatteryPct() {
    static int cached = -1;
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (cached < 0 || (now - last_ms) >= 3000) {
        cached = static_cast<int>(M5Cardputer.Power.getBatteryLevel());
        last_ms = now;
    }
    return cached;
}

uint16_t batteryColor(int pct) {
    if (pct < 0) return cfg::COL_GRAY;
    if (pct < 20) return cfg::COL_RED;
    if (pct < 40) return cfg::COL_YELLOW;
    return cfg::COL_GREEN;
}

std::string batteryText(int pct) {
    if (pct < 0) return "--%";
    return std::to_string(pct) + "%";
}

void drawWifiIcon(int x, bool connected) {
    int cx = x + 6;
    int cy = 14;
    uint16_t col = connected ? cfg::COL_GREEN : cfg::COL_GRAY;
    g_canvas.fillCircle(cx, cy, 1, col);
    g_canvas.fillArc(cx, cy, 4, 3, 225.0f, 315.0f, col);
    g_canvas.fillArc(cx, cy, 7, 6, 225.0f, 315.0f, col);
    g_canvas.fillArc(cx, cy, 10, 9, 225.0f, 315.0f, col);
    if (!connected) {
        g_canvas.drawLine(x, 4, x + 12, 15, cfg::COL_RED);
    }
}

void drawHeader(const char* title, const char* right, uint16_t right_col) {
    g_canvas.fillRect(0, 0, cfg::SCREEN_W, 20, cfg::COL_DARK);
    g_canvas.fillRect(0, 20, cfg::SCREEN_W, 1, cfg::COL_ORANGE);
    g_canvas.setTextSize(1);

    int pct = readBatteryPct();
    std::string bat = batteryText(pct);
    int bat_px = static_cast<int>(bat.size()) * 6;

    // Rightmost: battery %
    g_canvas.setTextDatum(top_right);
    g_canvas.setTextColor(batteryColor(pct), cfg::COL_DARK);
    g_canvas.drawString(bat.c_str(), cfg::SCREEN_W - 4, 6);

    // Left of battery: macOS-style Wi-Fi fan icon (green connected, gray+red slash offline)
    int icon_x = cfg::SCREEN_W - 4 - bat_px - 18;
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);
    drawWifiIcon(icon_x, wifi_ok);

    // Left of Wi-Fi icon: explicit right label or current HH:mm clock
    std::string tag = (right && *right) ? std::string(right) : time_sync::formatClockHm();
    uint16_t tag_col = (right && *right)
                           ? right_col
                           : (time_sync::isTimeSynced() ? cfg::COL_CREAM : cfg::COL_GRAY);
    int tag_x = icon_x - 5;
    int tag_px = static_cast<int>(tag.size()) * 6;
    g_canvas.setTextDatum(top_right);
    g_canvas.setTextColor(tag_col, cfg::COL_DARK);
    g_canvas.drawString(tag.c_str(), tag_x, 6);

    int title_max_px = std::max(36, tag_x - tag_px - 10);
    int max_title_chars = std::max(6, title_max_px / 6);
    g_canvas.setTextDatum(top_left);
    g_canvas.setTextColor(cfg::COL_ORANGE, cfg::COL_DARK);
    g_canvas.drawString(fitCols(title ? title : "", static_cast<size_t>(max_title_chars)).c_str(), 6, 6);
}

void drawHints(const char* left, const char* right) {
    g_canvas.fillRect(0, 117, cfg::SCREEN_W, 1, cfg::COL_ORANGE);
    g_canvas.fillRect(0, 118, cfg::SCREEN_W, 17, cfg::COL_DARK);
    g_canvas.setTextSize(1);
    g_canvas.setTextDatum(top_left);
    g_canvas.setTextColor(cfg::COL_CREAM, cfg::COL_DARK);
    g_canvas.drawString(left, 6, 122);
    if (right && *right) {
        g_canvas.setTextDatum(top_right);
        g_canvas.setTextColor(cfg::COL_GRAY, cfg::COL_DARK);
        g_canvas.drawString(right, cfg::SCREEN_W - 6, 122);
        g_canvas.setTextDatum(top_left);
    }
}

void showToast(const char* title, const std::string& line1, const std::string& line2,
               uint16_t title_col) {
    std::vector<std::pair<std::string, uint16_t>> rows;
    for (const auto& s : wrapLines(line1, 37)) {
        rows.push_back({s, title_col});
    }
    if (!line2.empty()) {
        for (const auto& s : wrapLines(line2, 37)) {
            rows.push_back({s, cfg::COL_CREAM});
        }
    }

    constexpr size_t kMaxVis = 7;
    size_t scroll = 0;
    bool scrollable = (rows.size() > kMaxVis);

    waitKeyRelease();
    while (true) {
        g_canvas.fillScreen(cfg::COL_BG);
        if (scrollable) {
            std::string right = std::to_string(scroll + 1) + "-" +
                                std::to_string(std::min<size_t>(scroll + kMaxVis, rows.size())) + "/" +
                                std::to_string(rows.size());
            drawHeader(title, right.c_str(), cfg::COL_GRAY);
        } else {
            drawHeader(title);
        }
        for (size_t i = 0; i < kMaxVis && (scroll + i) < rows.size(); ++i) {
            g_canvas.setTextColor(rows[scroll + i].second, cfg::COL_BG);
            g_canvas.drawString(rows[scroll + i].first.c_str(), 8, 26 + static_cast<int>(i) * 12);
        }
        if (scrollable) {
            drawHints(";. scroll", "Enter / Q close");
        } else {
            drawHints("Press any key...");
        }
        g_canvas.pushSprite(0, 0);

        KeyInput k = pollInput();
        if (scrollable) {
            applyMenuNav(k);
            if (k.up) {
                if (scroll > 0) --scroll;
            } else if (k.down) {
                if (scroll + kMaxVis < rows.size()) ++scroll;
            } else if (k.enter || k.esc || k.btn_g0 || k.del || k.ch != 0) {
                waitKeyRelease();
                return;
            }
        } else {
            if (k.enter || k.esc || k.btn_g0 || k.del || k.ch != 0) {
                waitKeyRelease();
                return;
            }
        }
        delay(25);
    }
}

bool promptConfirm(const char* title, const std::string& l1, const std::string& l2,
                   const std::string& l3) {
    waitKeyRelease();
    while (true) {
        g_canvas.fillScreen(cfg::COL_BG);
        drawHeader(title);
        g_canvas.setTextColor(cfg::COL_YELLOW, cfg::COL_BG);
        g_canvas.drawString(fitCols(l1, 37).c_str(), 8, 30);
        g_canvas.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        g_canvas.drawString(fitCols(l2, 37).c_str(), 8, 48);
        if (!l3.empty()) {
            g_canvas.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            g_canvas.drawString(fitCols(l3, 37).c_str(), 8, 66);
        }
        drawHints("Y confirm", "N / ESC cancel");
        g_canvas.pushSprite(0, 0);

        KeyInput k = pollInput();
        if (k.esc || k.btn_g0) {
            waitKeyRelease();
            return false;
        }
        if (k.ch != 0) {
            char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch)));
            if (lc == 'y') {
                waitKeyRelease();
                return true;
            }
            if (lc == 'n') {
                waitKeyRelease();
                return false;
            }
        }
        delay(25);
    }
}

bool promptText(const char* title, const std::string& subtitle, std::string& value,
                bool masked, size_t max_len) {
    waitKeyRelease();
    bool show_plain = false;
    bool blink = true;
    uint32_t last_blink = millis();
    size_t cursor = value.size();
    size_t view_start = 0;
    constexpr size_t kVisChars = 35;

    while (true) {
        if (millis() - last_blink > 450) {
            blink = !blink;
            last_blink = millis();
        }

        if (cursor > value.size()) cursor = value.size();
        if (value.size() <= kVisChars) {
            view_start = 0;
        } else {
            if (cursor < view_start) view_start = cursor;
            if (cursor > view_start + kVisChars) view_start = cursor - kVisChars;
        }

        g_canvas.fillScreen(cfg::COL_BG);
        if (masked) {
            drawHeader(title, show_plain ? "SHOW" : "MASK", show_plain ? cfg::COL_YELLOW : cfg::COL_GRAY);
        } else {
            drawHeader(title);
        }
        g_canvas.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        g_canvas.drawString(fitCols(subtitle, 37).c_str(), 8, 30);

        std::string full = (masked && !show_plain) ? std::string(value.size(), '*') : value;
        std::string shown = (view_start < full.size()) ? full.substr(view_start, kVisChars) : "";
        g_canvas.fillRect(6, 52, 228, 22, cfg::COL_DARK);
        g_canvas.drawRect(6, 52, 228, 22, cfg::COL_ORANGE);
        g_canvas.setTextColor(cfg::COL_CREAM, cfg::COL_DARK);
        g_canvas.drawString(shown.c_str(), 10, 59);
        if (blink) {
            int cx = 10 + static_cast<int>(cursor - view_start) * 6;
            if (cursor == value.size()) {
                g_canvas.fillRect(cx, 58, 5, 10, cfg::COL_ORANGE);
            } else {
                g_canvas.fillRect(cx - 1, 56, 2, 13, cfg::COL_ORANGE);
                g_canvas.fillRect(cx, 68, 6, 2, cfg::COL_ORANGE);
            }
        }
        if (masked) {
            drawHints(show_plain ? "Enter ok  Tab:Hide" : "Enter ok  Tab:Show", "Fn+`/G0 back");
        } else {
            drawHints("Enter save", "Fn+` / G0 cancel");
        }
        g_canvas.pushSprite(0, 0);

        KeyInput k = pollInput();
        if (k.esc || k.btn_g0) {
            waitKeyRelease();
            return false;
        }
        if (k.enter) {
            waitKeyRelease();
            return true;
        }
        if (k.left) {
            if (cursor > 0) --cursor;
            blink = true;
            last_blink = millis();
        } else if (k.right) {
            if (cursor < value.size()) ++cursor;
            blink = true;
            last_blink = millis();
        } else if (k.up) {
            cursor = 0;
            blink = true;
            last_blink = millis();
        } else if (k.down) {
            cursor = value.size();
            blink = true;
            last_blink = millis();
        } else if (masked && k.tab) {
            show_plain = !show_plain;
        } else if (k.del) {
            if (k.fn) {
                if (cursor < value.size()) {
                    value.erase(cursor, 1);
                    blink = true;
                    last_blink = millis();
                }
            } else if (cursor > 0) {
                value.erase(cursor - 1, 1);
                --cursor;
                blink = true;
                last_blink = millis();
            }
        } else if (!k.fn && !k.ctrl && !k.opt && k.ch >= 0x20 && k.ch < 0x7F && value.size() < max_len) {
            value.insert(cursor, 1, k.ch);
            ++cursor;
            blink = true;
            last_blink = millis();
        }
        delay(25);
    }
}

}  // namespace ui
