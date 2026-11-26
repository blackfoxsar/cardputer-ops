#pragma once

#include <array>
#include <stdint.h>

namespace sudoku_logic {

using Board = std::array<std::array<uint8_t, 9>, 9>;

struct Puzzle {
    Board board{};
    Board solution{};
};

Puzzle generatePuzzle(int difficulty, uint32_t seed);
bool isValidBoard(const Board& board);
bool isValidCompleteBoard(const Board& board);

}  // namespace sudoku_logic
