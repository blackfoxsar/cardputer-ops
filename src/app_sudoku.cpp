#include "app_sudoku.h"

#include <esp_system.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include "config.h"
#include "sudoku_logic.h"
#include "ui_common.h"

namespace {

constexpr int kCellW = 24;
constexpr int kCellH = 12;
constexpr int kGridX = 12;
constexpr int kGridY = 23;

int showDifficultyMenu() {
    const char* items[3] = {"1. Easy   (38 clues)", "2. Medium (30 clues)", "3. Hard   (24 clues)"};
    int cursor = 0;
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Sudoku");
        for (int i = 0; i < 3; ++i) {
            int y = 30 + i * 22;
            if (i == cursor) {
                cv.fillRect(8, y - 2, 224, 18, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(items[i], 14, y + 2);
        }
        ui::drawHints("Enter / 1-3 play", "Q / G0 back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return -1;
        }
        if (k.up) cursor = (cursor + 2) % 3;
        else if (k.down) cursor = (cursor + 1) % 3;
        else if (k.enter) {
            ui::waitKeyRelease();
            return cursor;
        } else if (k.ch >= '1' && k.ch <= '3') {
            ui::waitKeyRelease();
            return k.ch - '1';
        }
        delay(25);
    }
}

void drawSudokuGrid(const sudoku_logic::Board& board,
                    const std::array<std::array<bool, 9>, 9>& clues,
                    const std::array<std::array<int8_t, 9>, 9>* results,
                    int cr, int cc, const char* hdr_label, const char* hdr_right,
                    uint16_t right_col) {
    auto& cv = ui::canvas();
    cv.fillScreen(cfg::COL_BG);
    ui::drawHeader(hdr_label, hdr_right, right_col);

    for (int r = 0; r < 9; ++r) {
        for (int c = 0; c < 9; ++c) {
            int x = kGridX + c * kCellW;
            int y = kGridY + r * kCellH;
            bool is_cur = (r == cr && c == cc);

            uint16_t bg = cfg::COL_BG;
            if (results != nullptr && (*results)[r][c] != 0) {
                bg = ((*results)[r][c] > 0) ? cfg::COL_GREEN : cfg::COL_RED;
            } else if (is_cur) {
                bg = cfg::COL_DIM;
            }
            cv.fillRect(x, y, kCellW, kCellH, bg);

            uint8_t val = board[r][c];
            if (val != 0) {
                uint16_t fg = clues[r][c] ? cfg::COL_CREAM : cfg::COL_CYAN;
                if (results != nullptr && (*results)[r][c] != 0) {
                    fg = cfg::COL_BG;
                } else if (is_cur) {
                    fg = cfg::COL_ORANGE;
                }
                cv.setTextColor(fg, bg);
                char buf[2] = {static_cast<char>('0' + val), 0};
                cv.drawString(buf, x + (kCellW - 6) / 2, y + 2);
            }
        }
    }

    int gw = kCellW * 9;
    int gh = kCellH * 9;
    for (int i = 0; i <= 9; ++i) {
        int lw = (i % 3 == 0) ? 2 : 1;
        cv.fillRect(kGridX + i * kCellW, kGridY, lw, gh + 1, cfg::COL_GRAY);
        cv.fillRect(kGridX, kGridY + i * kCellH, gw + 2, lw, cfg::COL_GRAY);
    }
    cv.pushSprite(0, 0);
}

}  // namespace

void runSudokuApp() {
    while (true) {
        int diff = showDifficultyMenu();
        if (diff < 0) return;

        auto puzzle = sudoku_logic::generatePuzzle(diff, esp_random());
        std::array<std::array<bool, 9>, 9> clues{};
        for (int r = 0; r < 9; ++r) {
            for (int c = 0; c < 9; ++c) {
                clues[r][c] = (puzzle.board[r][c] != 0);
            }
        }

        const char* labels[3] = {"Sudoku [E]", "Sudoku [M]", "Sudoku [H]"};
        int cr = 0, cc = 0;
        bool has_results = false;
        std::array<std::array<int8_t, 9>, 9> results{};
        uint32_t results_at = 0;
        ui::waitKeyRelease();

        while (true) {
            if (has_results && (millis() - results_at >= 1200)) {
                has_results = false;
            }

            drawSudokuGrid(puzzle.board, clues, has_results ? &results : nullptr,
                           cr, cc, labels[diff], "Enter:Chk R:New", cfg::COL_GRAY);

            KeyInput k = ui::pollInput();
            ui::applyMenuNav(k);
            char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

            if (k.esc || k.btn_g0 || lc == 'q') {
                ui::waitKeyRelease();
                return;
            }
            if (lc == 'r') {
                ui::waitKeyRelease();
                break;
            }

            if (k.up || lc == 'w') cr = std::max(0, cr - 1);
            else if (k.down || lc == 's') cr = std::min(8, cr + 1);
            else if (k.left || lc == 'a') cc = std::max(0, cc - 1);
            else if (k.right || lc == 'd') cc = std::min(8, cc + 1);
            else if (k.ch >= '1' && k.ch <= '9') {
                if (!clues[cr][cc]) {
                    puzzle.board[cr][cc] = static_cast<uint8_t>(k.ch - '0');
                    has_results = false;
                }
            } else if (k.del || k.ch == '0') {
                if (!clues[cr][cc]) {
                    puzzle.board[cr][cc] = 0;
                    has_results = false;
                }
            } else if (k.enter) {
                if (sudoku_logic::isValidCompleteBoard(puzzle.board)) {
                    drawSudokuGrid(puzzle.board, clues, nullptr, -1, -1,
                                   "You Win!", "Any key -> menu", cfg::COL_GREEN);
                    ui::waitKeyRelease();
                    while (true) {
                        KeyInput wk = ui::pollInput();
                        if (wk.enter || wk.esc || wk.btn_g0 || wk.ch != 0) {
                            ui::waitKeyRelease();
                            break;
                        }
                        delay(25);
                    }
                    break;
                }
                for (int r = 0; r < 9; ++r) {
                    for (int c = 0; c < 9; ++c) {
                        if (clues[r][c]) results[r][c] = 1;
                        else if (puzzle.board[r][c] == 0) results[r][c] = 0;
                        else if (puzzle.board[r][c] == puzzle.solution[r][c]) results[r][c] = 1;
                        else results[r][c] = -1;
                    }
                }
                has_results = true;
                results_at = millis();
            }
            delay(25);
        }
    }
}
