#pragma once

#include <string>

namespace time_utils {

bool isLeapYear(int year);
int daysInMonth(int year, int month);
bool parseDateYmd(const std::string& s, int& year, int& month, int& day);
bool parseTimeHms(const std::string& s, int& hour, int& minute, int& second);
int clampTzOffset(int tz_hours);

}  // namespace time_utils
