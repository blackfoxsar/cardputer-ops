#include "app_time.h"

#include <M5Cardputer.h>

#include <cctype>
#include <cstdlib>
#include <string>

#include "config.h"
#include "storage.h"
#include "time_sync.h"
#include "time_utils.h"
#include "ui_common.h"

namespace {

uint16_t sourceBadgeColor(time_sync::TimeSource src) {
    switch (src) {
        case time_sync::TimeSource::NTP: return cfg::COL_GREEN;
        case time_sync::TimeSource::MANUAL: return cfg::COL_CYAN;
        default: return cfg::COL_RED;
    }
}

bool parseSignedInt(const std::string& raw, int& out) {
    size_t a = 0;
    while (a < raw.size() && std::isspace(static_cast<unsigned char>(raw[a]))) ++a;
    size_t b = raw.size();
    while (b > a && std::isspace(static_cast<unsigned char>(raw[b - 1]))) --b;
    if (a == b) return false;
    std::string t = raw.substr(a, b - a);
    size_t idx = 0;
    if (t[0] == '+' || t[0] == '-') idx = 1;
    if (idx >= t.size()) return false;
    for (size_t i = idx; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
    }
    out = std::atoi(t.c_str());
    return true;
}

}  // namespace

void runDateTimeApp() {
    int sel = 0;
    int scroll = 0;
    constexpr int kRows = 5;
    constexpr int kMaxVis = 4;
    AppSettings st = storage::loadSettings();
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        time_sync::TimeSource src = time_sync::timeSource();
        ui::drawHeader("Date & Time", time_sync::timeSourceLabel(), sourceBadgeColor(src));

        // Live clock banner
        std::string clk = (src != time_sync::TimeSource::UNSYNCED)
                              ? (time_sync::formatDisplayTime() + "  " + time_sync::formatTzLabel(st.tz_offset_hours))
                              : ("Not set  (" + time_sync::formatTzLabel(st.tz_offset_hours) + ")");
        cv.setTextColor((src != time_sync::TimeSource::UNSYNCED) ? cfg::COL_CYAN : cfg::COL_YELLOW, cfg::COL_BG);
        cv.drawString(ui::fitCols(clk, 37).c_str(), 10, 25);

        std::string date_str = (src != time_sync::TimeSource::UNSYNCED) ? time_sync::formatDateYmd() : "(not set)";
        std::string time_str = (src != time_sync::TimeSource::UNSYNCED) ? time_sync::formatTimeHms() : "(not set)";
        std::string row0 = "Sync NTP Now";
        std::string row1 = "Set Date: " + date_str;
        std::string row2 = "Set Time: " + time_str;
        std::string row3 = "Timezone: " + time_sync::formatTzLabel(st.tz_offset_hours);
        std::string row4 = "NTP Server: " + st.ntp_server;
        const std::string rows[kRows] = {row0, row1, row2, row3, row4};

        for (int i = 0; i < kMaxVis && (scroll + i) < kRows; ++i) {
            int idx = scroll + i;
            int y = 41 + i * 18;
            if (idx == sel) {
                cv.fillRect(6, y - 2, 228, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(ui::fitCols(rows[idx], 36).c_str(), 10, y + 2);
        }

        ui::drawHints(sel == 3 ? "Enter edit  ,/ step" : "Enter edit/sync", "Q / G0 back");
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
            if (sel < scroll) scroll = sel;
            if (sel >= scroll + kMaxVis) scroll = kRows - kMaxVis;
        } else if (k.down) {
            sel = (sel + 1) % kRows;
            if (sel >= scroll + kMaxVis) scroll = sel - kMaxVis + 1;
            if (sel < scroll) scroll = 0;
        } else if (sel == 3 && (k.left || k.right)) {
            st.tz_offset_hours += k.right ? 1 : -1;
            if (st.tz_offset_hours > 14) st.tz_offset_hours = -12;
            if (st.tz_offset_hours < -12) st.tz_offset_hours = 14;
            storage::saveSettings(st);
            time_sync::applyTimezone(st.tz_offset_hours);
        } else if (k.enter) {
            if (sel == 0) {
                std::string msg;
                bool ok = time_sync::syncWithUi(8000, &msg);
                ui::showToast(ok ? "NTP Synced" : "NTP Sync", msg, "",
                              ok ? cfg::COL_GREEN : cfg::COL_RED);
                ui::waitKeyRelease();
            } else if (sel == 1) {
                std::string val = time_sync::formatDateYmd();
                if (ui::promptText("Set Date", "Format: YYYY-MM-DD", val, false, 10)) {
                    int y = 0, m = 0, d = 0;
                    if (time_utils::parseDateYmd(val, y, m, d) && time_sync::setManualDate(y, m, d)) {
                        ui::showToast("Date Updated", time_sync::formatDisplayTime(), "", cfg::COL_GREEN);
                    } else {
                        ui::showToast("Invalid Date", "Use YYYY-MM-DD (2024-2099)", val, cfg::COL_RED);
                    }
                }
                ui::waitKeyRelease();
            } else if (sel == 2) {
                std::string val = time_sync::formatTimeHms();
                if (ui::promptText("Set Time", "Format: HH:MM or HH:MM:SS", val, false, 8)) {
                    int h = 0, m = 0, sec = 0;
                    if (time_utils::parseTimeHms(val, h, m, sec) && time_sync::setManualTime(h, m, sec)) {
                        ui::showToast("Time Updated", time_sync::formatDisplayTime(), "", cfg::COL_GREEN);
                    } else {
                        ui::showToast("Invalid Time", "Use 24h HH:MM or HH:MM:SS", val, cfg::COL_RED);
                    }
                }
                ui::waitKeyRelease();
            } else if (sel == 3) {
                std::string val = (st.tz_offset_hours >= 0 ? "+" : "") + std::to_string(st.tz_offset_hours);
                if (ui::promptText("Timezone (UTC)", "Offset hours: -12 to +14", val, false, 4)) {
                    int tz = 0;
                    if (parseSignedInt(val, tz) && tz >= -12 && tz <= 14) {
                        st.tz_offset_hours = tz;
                        storage::saveSettings(st);
                        time_sync::applyTimezone(st.tz_offset_hours);
                    } else {
                        ui::showToast("Invalid Timezone", "Enter integer -12 to +14", val, cfg::COL_RED);
                    }
                }
                ui::waitKeyRelease();
            } else if (sel == 4) {
                std::string srv = st.ntp_server;
                if (ui::promptText("NTP Server", "Hostname or IP address", srv, false, 63) && !srv.empty()) {
                    st.ntp_server = srv;
                    storage::saveSettings(st);
                }
                ui::waitKeyRelease();
            }
        }
        delay(25);
    }
}
