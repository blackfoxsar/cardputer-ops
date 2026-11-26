#include "time_sync.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include <cctype>
#include <cstdlib>

#include "config.h"
#include "storage.h"
#include "time_utils.h"
#include "ui_common.h"

namespace {

// lwIP SNTP keeps pointers passed to configTime(), so keep buffer static.
std::string g_ntp_server_buf = "pool.ntp.org";
int g_seq_counter = 0;
time_sync::TimeSource g_source = time_sync::TimeSource::UNSYNCED;

}  // namespace

namespace time_sync {

void applyTimezone(int tz_offset_hours) {
    int tz = time_utils::clampTzOffset(tz_offset_hours);
    // POSIX TZ sign is inverted relative to UTC offset: UTC+9 -> "UTC-9"
    char buf[16];
    if (tz == 0) {
        snprintf(buf, sizeof(buf), "UTC0");
    } else if (tz > 0) {
        snprintf(buf, sizeof(buf), "UTC-%d", tz);
    } else {
        snprintf(buf, sizeof(buf), "UTC+%d", -tz);
    }
    setenv("TZ", buf, 1);
    tzset();
}

void startAsyncSync() {
    AppSettings s = storage::loadSettings();
    int tz = time_utils::clampTzOffset(s.tz_offset_hours);
    g_ntp_server_buf = s.ntp_server.empty() ? "pool.ntp.org" : s.ntp_server;
    configTime(static_cast<long>(tz) * 3600L, 0,
               g_ntp_server_buf.c_str(), "time.cloudflare.com", "ntp.aliyun.com");
    applyTimezone(tz);
}

bool isTimeSynced() {
    return time(nullptr) >= 1704067200;  // 2024-01-01 00:00:00 UTC
}

TimeSource timeSource() {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        g_source = TimeSource::NTP;
    }
    if (g_source == TimeSource::MANUAL && isTimeSynced()) {
        return TimeSource::MANUAL;
    }
    if (isTimeSynced()) {
        return TimeSource::NTP;
    }
    return TimeSource::UNSYNCED;
}

const char* timeSourceLabel() {
    switch (timeSource()) {
        case TimeSource::NTP: return "NTP";
        case TimeSource::MANUAL: return "MANUAL";
        default: return "UNSYNC";
    }
}

bool setManualDate(int year, int month, int day) {
    AppSettings s = storage::loadSettings();
    applyTimezone(s.tz_offset_hours);

    struct tm tm_info{};
    if (isTimeSynced()) {
        time_t now = time(nullptr);
        localtime_r(&now, &tm_info);
    }
    tm_info.tm_year = year - 1900;
    tm_info.tm_mon = month - 1;
    tm_info.tm_mday = day;
    tm_info.tm_isdst = 0;

    time_t target = mktime(&tm_info);
    if (target < 1704067200) {
        return false;
    }
    struct timeval tv{};
    tv.tv_sec = target;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        return false;
    }
    g_source = TimeSource::MANUAL;
    return true;
}

bool setManualTime(int hour, int minute, int second) {
    AppSettings s = storage::loadSettings();
    applyTimezone(s.tz_offset_hours);

    struct tm tm_info{};
    if (isTimeSynced()) {
        time_t now = time(nullptr);
        localtime_r(&now, &tm_info);
    } else {
        tm_info.tm_year = 2026 - 1900;
        tm_info.tm_mon = 0;
        tm_info.tm_mday = 1;
    }
    tm_info.tm_hour = hour;
    tm_info.tm_min = minute;
    tm_info.tm_sec = second;
    tm_info.tm_isdst = 0;

    time_t target = mktime(&tm_info);
    if (target < 1704067200) {
        return false;
    }
    struct timeval tv{};
    tv.tv_sec = target;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        return false;
    }
    g_source = TimeSource::MANUAL;
    return true;
}

bool syncWithUi(uint32_t timeout_ms, std::string* out_msg) {
    if (WiFi.status() != WL_CONNECTED) {
        if (out_msg) *out_msg = "Wi-Fi offline";
        return false;
    }

    bool was_valid = isTimeSynced();
    AppSettings s = storage::loadSettings();
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    startAsyncSync();

    ui::waitKeyRelease();
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        bool got_ntp = (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) ||
                       (!was_valid && isTimeSynced());
        if (got_ntp) {
            g_source = TimeSource::NTP;
            if (out_msg) {
                *out_msg = formatDisplayTime() + " (" + formatTzLabel(s.tz_offset_hours) + ")";
            }
            return true;
        }

        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("NTP Time Sync");
        cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        cv.drawString("Syncing NTP...", 12, 36);
        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString(ui::fitCols("Server: " + g_ntp_server_buf, 36).c_str(), 12, 56);
        cv.drawString(("TZ: " + formatTzLabel(s.tz_offset_hours)).c_str(), 12, 74);
        ui::drawHints("Waiting NTP...", "Q / G0 cancel");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            if (out_msg) *out_msg = "Cancelled";
            ui::waitKeyRelease();
            return false;
        }
        delay(100);
    }

    if (out_msg) *out_msg = "NTP timeout";
    return false;
}

std::string formatDisplayTime() {
    if (!isTimeSynced()) {
        return "Not synced";
    }
    time_t now = time(nullptr);
    struct tm tm_info{};
    localtime_r(&now, &tm_info);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return buf;
}

std::string formatShortTime() {
    if (!isTimeSynced()) {
        return "No Time";
    }
    time_t now = time(nullptr);
    struct tm tm_info{};
    localtime_r(&now, &tm_info);
    char buf[16];
    strftime(buf, sizeof(buf), "%m-%d %H:%M", &tm_info);
    return buf;
}

std::string formatClockHm() {
    if (!isTimeSynced()) {
        return "--:--";
    }
    time_t now = time(nullptr);
    struct tm tm_info{};
    localtime_r(&now, &tm_info);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &tm_info);
    return buf;
}

std::string formatDateYmd() {
    if (!isTimeSynced()) {
        return "2026-01-01";
    }
    time_t now = time(nullptr);
    struct tm tm_info{};
    localtime_r(&now, &tm_info);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_info);
    return buf;
}

std::string formatTimeHms() {
    if (!isTimeSynced()) {
        return "00:00:00";
    }
    time_t now = time(nullptr);
    struct tm tm_info{};
    localtime_r(&now, &tm_info);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_info);
    return buf;
}

std::string formatLogFilenameStem(time_t t) {
    if (t >= 1704067200) {
        struct tm tm_info{};
        localtime_r(&t, &tm_info);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm_info);
        return buf;
    }
    // Fallback when clock has not been set yet: still matches yyyyMMdd_HHmmss.log shape
    uint32_t sec = millis() / 1000;
    uint32_t hh = (sec / 3600) % 24;
    uint32_t mm = (sec / 60) % 60;
    uint32_t ss = (sec + (++g_seq_counter - 1)) % 60;
    char buf[24];
    snprintf(buf, sizeof(buf), "19700101_%02lu%02lu%02lu",
             static_cast<unsigned long>(hh),
             static_cast<unsigned long>(mm),
             static_cast<unsigned long>(ss));
    return buf;
}

std::string formatTzLabel(int tz_offset_hours) {
    int tz = time_utils::clampTzOffset(tz_offset_hours);
    if (tz >= 0) {
        return "UTC+" + std::to_string(tz);
    }
    return "UTC" + std::to_string(tz);
}

}  // namespace time_sync
