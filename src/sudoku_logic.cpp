#include "sudoku_logic.h"

#include <algorithm>

namespace {

struct XorShift32 {
    uint32_t state;
    explicit XorShift32(uint32_t seed) : state(seed ? seed : 0x9E3779B9U) {}
    uint32_t next() {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }
    int range(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
};

constexpr sudoku_logic::Board kBaseSeed = {{
    {{5, 3, 4, 6, 7, 8, 9, 1, 2}},
    {{6, 7, 2, 1, 9, 5, 3, 4, 8}},
    {{1, 9, 8, 3, 4, 2, 5, 6, 7}},
    {{8, 5, 9, 7, 6, 1, 4, 2, 3}},
    {{4, 2, 6, 8, 5, 3, 7, 9, 1}},
    {{7, 1, 3, 9, 2, 4, 8, 5, 6}},
    {{9, 6, 1, 5, 3, 7, 2, 8, 4}},
    {{2, 8, 7, 4, 1, 9, 6, 3, 5}},
    {{3, 4, 5, 2, 8, 6, 1, 7, 9}},
}};

template <typename T, size_t N>
void shuffleArray(std::array<T, N>& arr, XorShift32& rng) {
    for (int i = static_cast<int>(N) - 1; i > 0; --i) {
        int j = rng.range(i + 1);
        std::swap(arr[i], arr[j]);
    }
}

void substituteDigits(sudoku_logic::Board& b, XorShift32& rng) {
    std::array<uint8_t, 9> digits{{1, 2, 3, 4, 5, 6, 7, 8, 9}};
    shuffleArray(digits, rng);
    for (int r = 0; r < 9; ++r) {
        for (int c = 0; c < 9; ++c) {
            b[r][c] = digits[b[r][c] - 1];
        }
    }
}

void swapRowsInBand(sudoku_logic::Board& b, int band, XorShift32& rng) {
    std::array<int, 3> order{{0, 1, 2}};
    shuffleArray(order, rng);
    int r0 = band * 3;
    std::array<std::array<uint8_t, 9>, 3> copy{{b[r0], b[r0 + 1], b[r0 + 2]}};
    for (int i = 0; i < 3; ++i) {
        b[r0 + i] = copy[order[i]];
    }
}

void swapColsInStack(sudoku_logic::Board& b, int stack, XorShift32& rng) {
    std::array<int, 3> order{{0, 1, 2}};
    shuffleArray(order, rng);
    int c0 = stack * 3;
    for (int r = 0; r < 9; ++r) {
        std::array<uint8_t, 3> copy{{b[r][c0], b[r][c0 + 1], b[r][c0 + 2]}};
        for (int i = 0; i < 3; ++i) {
            b[r][c0 + i] = copy[order[i]];
        }
    }
}

void swapBands(sudoku_logic::Board& b, XorShift32& rng) {
    std::array<int, 3> bands{{0, 1, 2}};
    shuffleArray(bands, rng);
    sudoku_logic::Board copy = b;
    for (int bi = 0; bi < 3; ++bi) {
        for (int r = 0; r < 3; ++r) {
            b[bi * 3 + r] = copy[bands[bi] * 3 + r];
        }
    }
}

void swapStacks(sudoku_logic::Board& b, XorShift32& rng) {
    std::array<int, 3> stacks{{0, 1, 2}};
    shuffleArray(stacks, rng);
    sudoku_logic::Board copy = b;
    for (int r = 0; r < 9; ++r) {
        for (int si = 0; si < 3; ++si) {
            for (int c = 0; c < 3; ++c) {
                b[r][si * 3 + c] = copy[r][stacks[si] * 3 + c];
            }
        }
    }
}

void transposeBoard(sudoku_logic::Board& b) {
    sudoku_logic::Board copy = b;
    for (int r = 0; r < 9; ++r) {
        for (int c = 0; c < 9; ++c) {
            b[r][c] = copy[c][r];
        }
    }
}

}  // namespace

namespace sudoku_logic {

Puzzle generatePuzzle(int difficulty, uint32_t seed) {
    XorShift32 rng(seed);
    Board b = kBaseSeed;

    substituteDigits(b, rng);
    for (int i = 0; i < 3; ++i) {
        swapRowsInBand(b, i, rng);
        swapColsInStack(b, i, rng);
    }
    swapBands(b, rng);
    swapStacks(b, rng);
    if (rng.range(2)) transposeBoard(b);
    if (rng.range(2)) {
        for (int r = 0; r < 9; ++r) std::reverse(b[r].begin(), b[r].end());
    }
    if (rng.range(2)) {
        for (int r = 0; r < 4; ++r) std::swap(b[r], b[8 - r]);
    }

    Puzzle p;
    p.solution = b;

    int clues = 30;
    if (difficulty == 0) clues = 38;
    else if (difficulty == 2) clues = 24;

    std::array<uint8_t, 81> positions{};
    for (uint8_t i = 0; i < 81; ++i) positions[i] = i;
    shuffleArray(positions, rng);

    int to_remove = 81 - clues;
    for (int i = 0; i < to_remove; ++i) {
        int pos = positions[i];
        b[pos / 9][pos % 9] = 0;
    }
    p.board = b;
    return p;
}

bool isValidBoard(const Board& board) {
    for (int r = 0; r < 9; ++r) {
        uint16_t mask = 0;
        for (int c = 0; c < 9; ++c) {
            uint8_t v = board[r][c];
            if (v > 9) return false;
            if (v) {
                uint16_t bit = static_cast<uint16_t>(1U << v);
                if (mask & bit) return false;
                mask |= bit;
            }
        }
    }
    for (int c = 0; c < 9; ++c) {
        uint16_t mask = 0;
        for (int r = 0; r < 9; ++r) {
            uint8_t v = board[r][c];
            if (v) {
                uint16_t bit = static_cast<uint16_t>(1U << v);
                if (mask & bit) return false;
                mask |= bit;
            }
        }
    }
    for (int br = 0; br < 3; ++br) {
        for (int bc = 0; bc < 3; ++bc) {
            uint16_t mask = 0;
            for (int r = br * 3; r < br * 3 + 3; ++r) {
                for (int c = bc * 3; c < bc * 3 + 3; ++c) {
                    uint8_t v = board[r][c];
                    if (v) {
                        uint16_t bit = static_cast<uint16_t>(1U << v);
                        if (mask & bit) return false;
                        mask |= bit;
                    }
                }
            }
        }
    }
    return true;
}

bool isValidCompleteBoard(const Board& board) {
    for (int r = 0; r < 9; ++r) {
        for (int c = 0; c < 9; ++c) {
            if (board[r][c] < 1 || board[r][c] > 9) return false;
        }
    }
    return isValidBoard(board);
}

}  // namespace sudoku_logic
