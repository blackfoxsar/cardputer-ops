#pragma once

#include <stdint.h>
#include <stddef.h>

class VT100Term {
public:
    static constexpr int kDefaultFg = 7; // Light gray / cream
    static constexpr int kDefaultBg = 0; // Black

    struct Cell {
        char ch = ' ';
        uint8_t fg = kDefaultFg;
        uint8_t bg = kDefaultBg;
        bool bold = false;
        bool inverse = false;
    };

    VT100Term(int cols = 40, int rows = 15);

    void reset();
    void feed(const char* data, size_t len);

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int cursorCol() const { return cur_col_; }
    int cursorRow() const { return cur_row_; }
    bool cursorVisible() const { return cursor_visible_; }

    const Cell& cell(int r, int c) const { return cells_[r * cols_ + c]; }
    bool isRowDirty(int r) const { return r >= 0 && r < rows_ && ((dirty_mask_ >> r) & 1U) != 0; }
    void clearDirty() { dirty_mask_ = 0; }
    void markAllDirty();

private:
    enum class State {
        TEXT,
        ESC,
        CSI,
        OSC,
        OSC_ESC,
        CHARSET,
    };

    int cols_;
    int rows_;
    int cur_col_ = 0;
    int cur_row_ = 0;
    int saved_col_ = 0;
    int saved_row_ = 0;
    int scroll_top_ = 0;
    int scroll_bottom_ = 14;
    bool pending_wrap_ = false;
    bool cursor_visible_ = true;

    uint8_t cur_fg_ = kDefaultFg;
    uint8_t cur_bg_ = kDefaultBg;
    bool cur_bold_ = false;
    bool cur_inverse_ = false;

    State state_ = State::TEXT;
    char csi_buf_[64]{};
    int csi_len_ = 0;
    int utf8_rem_ = 0;

    Cell cells_[40 * 24]{};
    uint32_t dirty_mask_ = 0;

    void markRowDirty(int r);
    void putChar(char ch);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int top, int bottom, int n = 1);
    void scrollDown(int top, int bottom, int n = 1);
    void clearCells(int r0, int c0, int r1, int c1);
    void handleCsi(char final_ch);
    void handleSgr(const int* params, int count);
};
