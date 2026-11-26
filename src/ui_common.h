#pragma once

#include <M5Cardputer.h>
#include <stdint.h>
#include <string>

struct KeyInput {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool enter = false;
    bool esc = false;
    bool del = false;
    bool tab = false;
    bool fn = false;
    bool ctrl = false;
    bool opt = false;
    bool btn_g0 = false;
    char ch = 0;
};

namespace ui {

void initCanvas();
M5Canvas& canvas();

void waitKeyRelease(uint32_t max_ms = 600);
KeyInput pollInput(uint32_t repeat_ms = 220);
void applyMenuNav(KeyInput& in);

std::string fitCols(const std::string& s, size_t max_chars);

int readBatteryPct();
uint16_t batteryColor(int pct);
std::string batteryText(int pct);

void drawHeader(const char* title, const char* right = nullptr, uint16_t right_col = 0x7BEF);
void drawHints(const char* left, const char* right = nullptr);

void showToast(const char* title, const std::string& line1, const std::string& line2 = "",
               uint16_t title_col = 0xCBAB);
bool promptConfirm(const char* title, const std::string& l1, const std::string& l2,
                   const std::string& l3 = "");
bool promptText(const char* title, const std::string& subtitle, std::string& value,
                bool masked = false, size_t max_len = 64);

}  // namespace ui
