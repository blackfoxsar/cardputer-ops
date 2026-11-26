#pragma once

#include <ctime>
#include <stdint.h>
#include <string>

namespace time_sync {

enum class TimeSource {
    UNSYNCED,
    MANUAL,
    NTP,
};

void applyTimezone(int tz_offset_hours);
void startAsyncSync();
bool isTimeSynced();
TimeSource timeSource();
const char* timeSourceLabel();

bool setManualDate(int year, int month, int day);
bool setManualTime(int hour, int minute, int second);
bool syncWithUi(uint32_t timeout_ms = 8000, std::string* out_msg = nullptr);

std::string formatDisplayTime();
std::string formatShortTime();
std::string formatClockHm();
std::string formatDateYmd();
std::string formatHeaderDate(int max_chars);
std::string formatTimeHms();
std::string formatLogFilenameStem(time_t t);
std::string formatTzLabel(int tz_offset_hours);

}  // namespace time_sync
