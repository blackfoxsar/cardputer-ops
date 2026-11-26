#include "app_alarm.h"

#include <LittleFS.h>
#include <M5Cardputer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "audio_hw.h"
#include "audio_utils.h"
#include "config.h"
#include "time_sync.h"
#include "ui_common.h"

namespace {

constexpr size_t kMaxAlarms = 6;

struct AlarmItem {
    int hour = 7;
    int minute = 0;
    bool enabled = true;
    bool repeat_daily = true;
    std::string label = "Alarm";
};

std::vector<AlarmItem> g_alarms;
int g_alarm_vol_pct = 75;
int g_suppress_depth = 0;
bool g_in_alarm_modal = false;
bool g_loaded = false;
int g_last_trig_key = -1;
time_t g_snooze_until = 0;
std::string g_snooze_label;

std::string sanitizeLabel(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (c == '\r' || c == '\n' || c == ',') out.push_back(' ');
        else if (c >= 0x20 && c < 0x7F) out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) out = "Alarm";
    if (out.size() > 24) out.resize(24);
    return out;
}

void saveAlarms() {
    File f = LittleFS.open(cfg::FILE_ALARMS, "w");
    if (!f) return;
    f.printf("VOL=%d\n", std::max(0, std::min(100, g_alarm_vol_pct)));
    for (const auto& a : g_alarms) {
        f.printf("%d,%d,%d,%d,%s\n",
                 a.hour, a.minute, a.enabled ? 1 : 0, a.repeat_daily ? 1 : 0,
                 sanitizeLabel(a.label).c_str());
    }
    f.close();
}

void loadAlarms() {
    g_loaded = true;
    g_alarms.clear();
    g_alarm_vol_pct = 75;
    if (!LittleFS.exists(cfg::FILE_ALARMS)) return;

    File f = LittleFS.open(cfg::FILE_ALARMS, "r");
    if (!f) return;

    while (f.available() && g_alarms.size() < kMaxAlarms) {
        String line_s = f.readStringUntil('\n');
        std::string line = line_s.c_str();
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;

        if (line.rfind("VOL=", 0) == 0) {
            int v = std::atoi(line.substr(4).c_str());
            g_alarm_vol_pct = std::max(0, std::min(100, v));
            continue;
        }

        // Parse hh,mm,en,rep,label
        int h = 0, m = 0, en = 1, rep = 1;
        size_t c1 = line.find(',');
        size_t c2 = (c1 == std::string::npos) ? std::string::npos : line.find(',', c1 + 1);
        size_t c3 = (c2 == std::string::npos) ? std::string::npos : line.find(',', c2 + 1);
        size_t c4 = (c3 == std::string::npos) ? std::string::npos : line.find(',', c3 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos ||
            c3 == std::string::npos || c4 == std::string::npos) {
            continue;
        }
        h = std::atoi(line.substr(0, c1).c_str());
        m = std::atoi(line.substr(c1 + 1, c2 - c1 - 1).c_str());
        en = std::atoi(line.substr(c2 + 1, c3 - c2 - 1).c_str());
        rep = std::atoi(line.substr(c3 + 1, c4 - c3 - 1).c_str());

        AlarmItem item;
        item.hour = std::max(0, std::min(23, h));
        item.minute = std::max(0, std::min(59, m));
        item.enabled = (en != 0);
        item.repeat_daily = (rep != 0);
        item.label = sanitizeLabel(line.substr(c4 + 1));
        g_alarms.push_back(item);
    }
    f.close();
}

const int16_t* getAlarmBeepBuf() {
    static int16_t buf[480];  // 10ms @ 48kHz, 1.5 kHz (15 cycles = 32 samples/cycle -> 480 = exact!)
    static bool ready = false;
    if (!ready) {
        constexpr double kTwoPi = 6.28318530717958647692;
        for (int i = 0; i < 480; ++i) {
            buf[i] = static_cast<int16_t>(std::sin(kTwoPi * i / 24.0) * 24000.0);  // 2 kHz exact
        }
        ready = true;
    }
    return buf;
}

std::string formatHm(int h, int m) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    return buf;
}

void triggerRingingModal(const std::string& label, const std::string& hm_str) {
    g_in_alarm_modal = true;
    int vol_pct = std::max(20, g_alarm_vol_pct);
    bool spk_ok = audio_hw::prepareSpeaker(vol_pct);
    const int16_t* beep = getAlarmBeepBuf();

    ui::waitKeyRelease();
    uint32_t last_phase_ms = millis();
    bool tone_on = false;
    bool flash = false;

    while (true) {
        uint32_t now = millis();
        if (now - last_phase_ms >= 350) {
            last_phase_ms = now;
            flash = !flash;
            if (spk_ok) {
                tone_on = !tone_on;
                if (tone_on) {
                    M5Cardputer.Speaker.playRaw(beep, 480, 48000, false, UINT32_MAX, 0, true);
                } else {
                    M5Cardputer.Speaker.stop(0);
                }
            }
        }

        auto& cv = ui::canvas();
        cv.fillScreen(flash ? cfg::COL_DARK : cfg::COL_BG);
        ui::drawHeader("ALARM");

        cv.setTextDatum(top_center);
        cv.setTextColor(flash ? cfg::COL_YELLOW : cfg::COL_RED, flash ? cfg::COL_DARK : cfg::COL_BG);
        cv.setTextSize(2);
        cv.drawString(hm_str.c_str(), cfg::SCREEN_W / 2, 34);
        cv.setTextSize(1);

        cv.setTextColor(cfg::COL_CREAM, flash ? cfg::COL_DARK : cfg::COL_BG);
        cv.drawString(ui::fitCols(label, 34).c_str(), cfg::SCREEN_W / 2, 64);
        cv.setTextDatum(top_left);

        ui::drawHints("Enter / ESC stop", "Space snooze +5m");
        cv.pushSprite(0, 0);

        M5Cardputer.update();
        if (M5Cardputer.BtnA.wasPressed()) {
            break;
        }
        auto st = M5Cardputer.Keyboard.keysState();
        bool is_esc = (st.fn && !st.word.empty() && (st.word[0] == '`' || st.word[0] == '~'));
        if (st.enter || is_esc) {
            break;
        }
        if (!st.word.empty() && st.word[0] == ' ') {
            g_snooze_until = time(nullptr) + 300;
            g_snooze_label = label + " (Snooze)";
            break;
        }
        delay(25);
    }

    audio_hw::restoreQuietSpeakerIdle();
    ui::waitKeyRelease();
    g_in_alarm_modal = false;
}

void runTestAlarmTone(int vol_pct) {
    app_alarm::SuppressGuard guard;
    if (!audio_hw::prepareSpeaker(vol_pct)) {
        ui::showToast("Speaker Error", "Failed to start ES8311", "", cfg::COL_RED);
        return;
    }
    const int16_t* beep = getAlarmBeepBuf();
    M5Cardputer.Speaker.playRaw(beep, 480, 48000, false, 30, 0, true);
    delay(320);
    audio_hw::restoreQuietSpeakerIdle();
    ui::waitKeyRelease();
}

bool editAlarmDialog(AlarmItem& item, bool is_new) {
    int cursor = 0;
    constexpr int kRows = 6;
    ui::waitKeyRelease();

    while (true) {
        const std::string rows[kRows] = {
            "Hour:   " + formatHm(item.hour, item.minute).substr(0, 2) + "   (,/. step)",
            "Minute: " + formatHm(item.hour, item.minute).substr(3, 2) + "   (,/. step)",
            std::string("Repeat: ") + (item.repeat_daily ? "Daily" : "Once"),
            "Label:  " + item.label,
            "Status: " + std::string(item.enabled ? "ON" : "OFF"),
            "[ Save Alarm ]"
        };

        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader(is_new ? "New Alarm" : "Edit Alarm");

        for (int i = 0; i < kRows; ++i) {
            int y = 23 + i * 15;
            if (i == cursor) {
                cv.fillRect(6, y - 1, 228, 14, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(i == 5 ? cfg::COL_GREEN : cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(ui::fitCols(rows[i], 36).c_str(), 10, y + 2);
        }

        ui::drawHints(cursor <= 1 ? ",/ adjust  Enter toggle" : "Enter edit  T test", "ESC cancel");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput(75);
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0) {
            ui::waitKeyRelease();
            return false;
        }
        if (k.up) cursor = (cursor + kRows - 1) % kRows;
        else if (k.down) cursor = (cursor + 1) % kRows;
        else if (cursor == 0 && (k.left || k.right)) {
            int delta = k.right ? 1 : -1;
            item.hour = (item.hour + delta + 24) % 24;
        } else if (cursor == 1 && (k.left || k.right)) {
            int step = (k.repeat_count >= 10) ? 5 : 1;
            int delta = k.right ? step : -step;
            item.minute = (item.minute + delta + 60) % 60;
        } else if (cursor == 2 && (k.left || k.right || k.enter || k.ch == ' ')) {
            item.repeat_daily = !item.repeat_daily;
        } else if (cursor == 4 && (k.left || k.right || k.enter || k.ch == ' ')) {
            item.enabled = !item.enabled;
        } else if (lc == 't') {
            runTestAlarmTone(g_alarm_vol_pct);
        } else if (k.enter) {
            if (cursor == 0) {
                std::string s = std::to_string(item.hour);
                if (ui::promptText("Alarm Hour", "0 - 23", s, false, 2)) {
                    int h = std::atoi(s.c_str());
                    if (h >= 0 && h <= 23) item.hour = h;
                }
                ui::waitKeyRelease();
            } else if (cursor == 1) {
                std::string s = std::to_string(item.minute);
                if (ui::promptText("Alarm Minute", "0 - 59", s, false, 2)) {
                    int m = std::atoi(s.c_str());
                    if (m >= 0 && m <= 59) item.minute = m;
                }
                ui::waitKeyRelease();
            } else if (cursor == 3) {
                std::string s = item.label;
                if (ui::promptText("Alarm Label", "Short note (max 24 chars)", s, false, 24)) {
                    item.label = sanitizeLabel(s);
                }
                ui::waitKeyRelease();
            } else if (cursor == 5) {
                ui::waitKeyRelease();
                return true;
            }
        }
        delay(20);
    }
}

}  // namespace

namespace app_alarm {

void init() {
    if (!g_loaded) {
        loadAlarms();
    }
}

void setSuppressed(bool suppressed) {
    if (suppressed) {
        ++g_suppress_depth;
    } else if (g_suppress_depth > 0) {
        --g_suppress_depth;
    }
}

bool isSuppressed() {
    return g_suppress_depth > 0 || g_in_alarm_modal;
}

void checkBackground() {
    if (!g_loaded) loadAlarms();
    if (g_in_alarm_modal || g_suppress_depth > 0) return;

    static uint32_t s_last_poll_ms = 0;
    uint32_t now_ms = millis();
    if (now_ms - s_last_poll_ms < 400) return;
    s_last_poll_ms = now_ms;

    if (!time_sync::isTimeSynced()) return;
    time_t now = time(nullptr);
    struct tm tm_now{};
    localtime_r(&now, &tm_now);

    if (g_snooze_until > 0 && now >= g_snooze_until) {
        std::string lbl = g_snooze_label.empty() ? "Alarm (Snooze)" : g_snooze_label;
        g_snooze_until = 0;
        triggerRingingModal(lbl, formatHm(tm_now.tm_hour, tm_now.tm_min));
        return;
    }

    int cur_key = (tm_now.tm_yday * 1440) + tm_now.tm_hour * 60 + tm_now.tm_min;
    if (cur_key == g_last_trig_key) return;

    for (size_t i = 0; i < g_alarms.size(); ++i) {
        auto& a = g_alarms[i];
        if (a.enabled && a.hour == tm_now.tm_hour && a.minute == tm_now.tm_min) {
            g_last_trig_key = cur_key;
            std::string lbl = a.label;
            std::string hm = formatHm(a.hour, a.minute);
            if (!a.repeat_daily) {
                a.enabled = false;
                saveAlarms();
            }
            g_snooze_until = 0;
            triggerRingingModal(lbl, hm);
            return;
        }
    }
}

}  // namespace app_alarm

void runAlarmApp() {
    app_alarm::init();
    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 4;
    ui::waitKeyRelease();

    while (true) {
        bool can_add = g_alarms.size() < kMaxAlarms;
        int total_rows = (can_add ? 1 : 0) + 1 + static_cast<int>(g_alarms.size());
        if (cursor >= total_rows) cursor = std::max(0, total_rows - 1);

        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Alarm");

        cv.setTextColor(time_sync::isTimeSynced() ? cfg::COL_CYAN : cfg::COL_YELLOW, cfg::COL_BG);
        std::string top_info = time_sync::isTimeSynced()
                                   ? ("Now: " + time_sync::formatDisplayTime())
                                   : "Set clock in Settings -> Date & Time";
        cv.drawString(ui::fitCols(top_info, 37).c_str(), 8, 24);

        for (int i = 0; i < kMaxVis && (scroll + i) < total_rows; ++i) {
            int row = scroll + i;
            int y = 40 + i * 18;
            bool sel = (row == cursor);
            if (sel) {
                cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }

            int vol_row = can_add ? 1 : 0;
            int alarm_base = vol_row + 1;

            if (can_add && row == 0) {
                if (!sel) cv.setTextColor(cfg::COL_GREEN, cfg::COL_BG);
                cv.drawString("[ + Add Alarm ]", 10, y + 2);
            } else if (row == vol_row) {
                if (!sel) cv.setTextColor(cfg::COL_CYAN, cfg::COL_BG);
                cv.drawString(("Volume: " + std::to_string(g_alarm_vol_pct) + "%  (,/. +/-1%)").c_str(), 10, y + 2);
            } else {
                const auto& a = g_alarms[row - alarm_base];
                std::string left = formatHm(a.hour, a.minute) + " " + a.label;
                cv.drawString(ui::fitCols(left, 25).c_str(), 8, y + 2);
                if (!sel) cv.setTextColor(a.enabled ? cfg::COL_GREEN : cfg::COL_GRAY, cfg::COL_BG);
                cv.setTextDatum(top_right);
                std::string tag = std::string(a.repeat_daily ? "D " : "1x ") + (a.enabled ? "ON" : "OFF");
                cv.drawString(tag.c_str(), 232, y + 2);
                cv.setTextDatum(top_left);
            }
        }

        int vol_row = can_add ? 1 : 0;
        int alarm_base = vol_row + 1;
        bool on_alarm = (cursor >= alarm_base);
        const char* hint_l = (cursor == vol_row)
                                 ? ",/. vol±1%  Enter test"
                                 : (on_alarm ? "Spc ON/OFF En edit D del" : "Enter add alarm");
        ui::drawHints(hint_l, "ESC / G0 back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput(65);
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0) {
            ui::waitKeyRelease();
            return;
        }
        if (k.up) {
            cursor = (cursor - 1 + total_rows) % total_rows;
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, total_rows - kMaxVis);
        } else if (k.down) {
            cursor = (cursor + 1) % total_rows;
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (cursor == vol_row && (k.left || k.right || k.ch == '-' || k.ch == '+' || k.ch == '=')) {
            int dir = (k.right || k.ch == '+' || k.ch == '=') ? +1 : -1;
            g_alarm_vol_pct = audio_utils::stepVolumePct(g_alarm_vol_pct, dir, k.repeat_count);
            saveAlarms();
        } else if (cursor == vol_row && k.enter) {
            runTestAlarmTone(g_alarm_vol_pct);
        } else if (can_add && cursor == 0 && k.enter) {
            AlarmItem na;
            if (editAlarmDialog(na, true)) {
                g_alarms.push_back(na);
                saveAlarms();
            }
            ui::waitKeyRelease();
        } else if (on_alarm) {
            int aidx = cursor - alarm_base;
            if (k.ch == ' ') {
                g_alarms[aidx].enabled = !g_alarms[aidx].enabled;
                saveAlarms();
            } else if (k.enter) {
                AlarmItem copy = g_alarms[aidx];
                if (editAlarmDialog(copy, false)) {
                    g_alarms[aidx] = copy;
                    saveAlarms();
                }
                ui::waitKeyRelease();
            } else if (lc == 'd') {
                if (ui::promptConfirm("Delete Alarm?", formatHm(g_alarms[aidx].hour, g_alarms[aidx].minute),
                                      g_alarms[aidx].label)) {
                    g_alarms.erase(g_alarms.begin() + aidx);
                    saveAlarms();
                }
                ui::waitKeyRelease();
            }
        }
        delay(20);
    }
}
