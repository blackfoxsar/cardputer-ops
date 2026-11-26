#include "time_utils.h"

#include <cctype>

namespace {

std::string trimAscii(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool parseAllDigits(const std::string& s, int& out) {
    if (s.empty()) return false;
    int val = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        val = val * 10 + (c - '0');
    }
    out = val;
    return true;
}

}  // namespace

namespace time_utils {

bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int daysInMonth(int year, int month) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && isLeapYear(year)) return 29;
    return kDays[month - 1];
}

bool parseDateYmd(const std::string& s, int& year, int& month, int& day) {
    std::string t = trimAscii(s);
    int y = 0, m = 0, d = 0;

    if (t.size() == 8 && t.find_first_of("-/") == std::string::npos) {
        if (!parseAllDigits(t.substr(0, 4), y) ||
            !parseAllDigits(t.substr(4, 2), m) ||
            !parseAllDigits(t.substr(6, 2), d)) {
            return false;
        }
    } else {
        char sep = (t.find('-') != std::string::npos) ? '-' : '/';
        size_t p1 = t.find(sep);
        if (p1 == std::string::npos) return false;
        size_t p2 = t.find(sep, p1 + 1);
        if (p2 == std::string::npos || t.find(sep, p2 + 1) != std::string::npos) return false;
        if (!parseAllDigits(t.substr(0, p1), y) ||
            !parseAllDigits(t.substr(p1 + 1, p2 - p1 - 1), m) ||
            !parseAllDigits(t.substr(p2 + 1), d)) {
            return false;
        }
    }

    if (y < 2024 || y > 2099 || m < 1 || m > 12) return false;
    int max_d = daysInMonth(y, m);
    if (d < 1 || d > max_d) return false;
    year = y;
    month = m;
    day = d;
    return true;
}

bool parseTimeHms(const std::string& s, int& hour, int& minute, int& second) {
    std::string t = trimAscii(s);
    int h = 0, m = 0, sec = 0;

    if (t.find(':') == std::string::npos) {
        if (t.size() == 4) {
            if (!parseAllDigits(t.substr(0, 2), h) ||
                !parseAllDigits(t.substr(2, 2), m)) {
                return false;
            }
        } else if (t.size() == 6) {
            if (!parseAllDigits(t.substr(0, 2), h) ||
                !parseAllDigits(t.substr(2, 2), m) ||
                !parseAllDigits(t.substr(4, 2), sec)) {
                return false;
            }
        } else {
            return false;
        }
    } else {
        size_t c1 = t.find(':');
        size_t c2 = t.find(':', c1 + 1);
        if (c2 == std::string::npos) {
            if (!parseAllDigits(t.substr(0, c1), h) ||
                !parseAllDigits(t.substr(c1 + 1), m)) {
                return false;
            }
        } else {
            if (t.find(':', c2 + 1) != std::string::npos) return false;
            if (!parseAllDigits(t.substr(0, c1), h) ||
                !parseAllDigits(t.substr(c1 + 1, c2 - c1 - 1), m) ||
                !parseAllDigits(t.substr(c2 + 1), sec)) {
                return false;
            }
        }
    }

    if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return false;
    hour = h;
    minute = m;
    second = sec;
    return true;
}

int clampTzOffset(int tz_hours) {
    if (tz_hours < -12) return -12;
    if (tz_hours > 14) return 14;
    return tz_hours;
}

}  // namespace time_utils
