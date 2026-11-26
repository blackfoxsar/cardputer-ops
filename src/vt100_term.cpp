#include "vt100_term.h"

#include <algorithm>
#include <cstdlib>

VT100Term::VT100Term(int cols, int rows)
    : cols_(cols > 40 ? 40 : (cols < 1 ? 1 : cols)),
      rows_(rows > 24 ? 24 : (rows < 1 ? 1 : rows)) {
    reset();
}

void VT100Term::reset() {
    cur_col_ = 0;
    cur_row_ = 0;
    saved_col_ = 0;
    saved_row_ = 0;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    pending_wrap_ = false;
    cursor_visible_ = true;
    cur_fg_ = kDefaultFg;
    cur_bg_ = kDefaultBg;
    cur_bold_ = false;
    cur_inverse_ = false;
    state_ = State::TEXT;
    csi_len_ = 0;
    utf8_rem_ = 0;
    clearCells(0, 0, rows_ - 1, cols_ - 1);
    markAllDirty();
}

void VT100Term::markRowDirty(int r) {
    if (r >= 0 && r < rows_) {
        dirty_mask_ |= (1U << r);
    }
}

void VT100Term::markAllDirty() {
    for (int r = 0; r < rows_; ++r) {
        markRowDirty(r);
    }
}

void VT100Term::clearCells(int r0, int c0, int r1, int c1) {
    for (int r = std::max(0, r0); r <= std::min(rows_ - 1, r1); ++r) {
        int start_c = (r == r0) ? std::max(0, c0) : 0;
        int end_c = (r == r1) ? std::min(cols_ - 1, c1) : (cols_ - 1);
        for (int c = start_c; c <= end_c; ++c) {
            Cell& cell = cells_[r * cols_ + c];
            cell.ch = ' ';
            cell.fg = kDefaultFg;
            cell.bg = cur_bg_;
            cell.bold = false;
            cell.inverse = false;
        }
        markRowDirty(r);
    }
}

void VT100Term::scrollUp(int top, int bottom, int n) {
    if (top < 0) top = 0;
    if (bottom >= rows_) bottom = rows_ - 1;
    if (top >= bottom || n <= 0) return;

    int count = std::min(n, bottom - top + 1);
    for (int r = top; r <= bottom - count; ++r) {
        for (int c = 0; c < cols_; ++c) {
            cells_[r * cols_ + c] = cells_[(r + count) * cols_ + c];
        }
        markRowDirty(r);
    }
    clearCells(bottom - count + 1, 0, bottom, cols_ - 1);
}

void VT100Term::scrollDown(int top, int bottom, int n) {
    if (top < 0) top = 0;
    if (bottom >= rows_) bottom = rows_ - 1;
    if (top >= bottom || n <= 0) return;

    int count = std::min(n, bottom - top + 1);
    for (int r = bottom; r >= top + count; --r) {
        for (int c = 0; c < cols_; ++c) {
            cells_[r * cols_ + c] = cells_[(r - count) * cols_ + c];
        }
        markRowDirty(r);
    }
    clearCells(top, 0, top + count - 1, cols_ - 1);
}

void VT100Term::lineFeed() {
    pending_wrap_ = false;
    if (cur_row_ == scroll_bottom_) {
        scrollUp(scroll_top_, scroll_bottom_, 1);
    } else if (cur_row_ < rows_ - 1) {
        ++cur_row_;
    }
}

void VT100Term::reverseIndex() {
    pending_wrap_ = false;
    if (cur_row_ == scroll_top_) {
        scrollDown(scroll_top_, scroll_bottom_, 1);
    } else if (cur_row_ > 0) {
        --cur_row_;
    }
}

void VT100Term::putChar(char ch) {
    if (pending_wrap_) {
        cur_col_ = 0;
        lineFeed();
        pending_wrap_ = false;
    }
    Cell& cell = cells_[cur_row_ * cols_ + cur_col_];
    cell.ch = ch;
    cell.fg = cur_fg_;
    cell.bg = cur_bg_;
    cell.bold = cur_bold_;
    cell.inverse = cur_inverse_;
    markRowDirty(cur_row_);

    if (cur_col_ + 1 >= cols_) {
        pending_wrap_ = true;
    } else {
        ++cur_col_;
    }
}

void VT100Term::handleSgr(const int* params, int count) {
    if (count == 0) {
        cur_fg_ = kDefaultFg;
        cur_bg_ = kDefaultBg;
        cur_bold_ = false;
        cur_inverse_ = false;
        return;
    }
    for (int i = 0; i < count; ++i) {
        int p = params[i];
        if (p == 0) {
            cur_fg_ = kDefaultFg;
            cur_bg_ = kDefaultBg;
            cur_bold_ = false;
            cur_inverse_ = false;
        } else if (p == 1) {
            cur_bold_ = true;
        } else if (p == 22) {
            cur_bold_ = false;
        } else if (p == 7) {
            cur_inverse_ = true;
        } else if (p == 27) {
            cur_inverse_ = false;
        } else if (p >= 30 && p <= 37) {
            cur_fg_ = static_cast<uint8_t>(p - 30);
        } else if (p == 39) {
            cur_fg_ = kDefaultFg;
        } else if (p >= 40 && p <= 47) {
            cur_bg_ = static_cast<uint8_t>(p - 40);
        } else if (p == 49) {
            cur_bg_ = kDefaultBg;
        } else if (p >= 90 && p <= 97) {
            cur_fg_ = static_cast<uint8_t>((p - 90) + 8);
        } else if (p >= 100 && p <= 107) {
            cur_bg_ = static_cast<uint8_t>((p - 100) + 8);
        } else if (p == 38 || p == 48) {
            // Skip 256-color (5;n) or 24-bit (2;r;g;b) extra args gracefully
            if (i + 1 < count) {
                int mode = params[i + 1];
                if (mode == 5 && i + 2 < count) {
                    if (p == 38) cur_fg_ = static_cast<uint8_t>(params[i + 2] & 0x0F);
                    else cur_bg_ = static_cast<uint8_t>(params[i + 2] & 0x0F);
                    i += 2;
                } else if (mode == 2 && i + 4 < count) {
                    i += 4;
                }
            }
        }
    }
}

void VT100Term::handleCsi(char final_ch) {
    bool priv = (csi_len_ > 0 && csi_buf_[0] == '?');
    const char* p = priv ? (csi_buf_ + 1) : csi_buf_;

    int params[16]{};
    int count = 0;
    if (*p != '\0') {
        int val = 0;
        bool has_digit = false;
        for (; *p != '\0'; ++p) {
            if (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                has_digit = true;
            } else if (*p == ';') {
                if (count < 16) params[count++] = has_digit ? val : 0;
                val = 0;
                has_digit = false;
            }
        }
        if (count < 16) params[count++] = has_digit ? val : 0;
    }

    auto p1 = [&](int def) -> int { return (count >= 1 && params[0] > 0) ? params[0] : def; };
    auto p2 = [&](int def) -> int { return (count >= 2 && params[1] > 0) ? params[1] : def; };

    if (priv) {
        if (final_ch == 'h' || final_ch == 'l') {
            bool enable = (final_ch == 'h');
            for (int i = 0; i < count; ++i) {
                if (params[i] == 25) {
                    cursor_visible_ = enable;
                } else if (params[i] == 1049 || params[i] == 47) {
                    if (enable) clearCells(0, 0, rows_ - 1, cols_ - 1);
                }
            }
        }
        return;
    }

    switch (final_ch) {
        case 'A':
            pending_wrap_ = false;
            cur_row_ = std::max(scroll_top_, cur_row_ - p1(1));
            break;
        case 'B':
            pending_wrap_ = false;
            cur_row_ = std::min(scroll_bottom_, cur_row_ + p1(1));
            break;
        case 'C':
            pending_wrap_ = false;
            cur_col_ = std::min(cols_ - 1, cur_col_ + p1(1));
            break;
        case 'D':
            pending_wrap_ = false;
            cur_col_ = std::max(0, cur_col_ - p1(1));
            break;
        case 'G':
            pending_wrap_ = false;
            cur_col_ = std::min(cols_ - 1, std::max(0, p1(1) - 1));
            break;
        case 'd':
            pending_wrap_ = false;
            cur_row_ = std::min(rows_ - 1, std::max(0, p1(1) - 1));
            break;
        case 'H':
        case 'f':
            pending_wrap_ = false;
            cur_row_ = std::min(rows_ - 1, std::max(0, p1(1) - 1));
            cur_col_ = std::min(cols_ - 1, std::max(0, p2(1) - 1));
            break;
        case 'J': {
            int mode = count > 0 ? params[0] : 0;
            if (mode == 0) clearCells(cur_row_, cur_col_, rows_ - 1, cols_ - 1);
            else if (mode == 1) clearCells(0, 0, cur_row_, cur_col_);
            else if (mode == 2 || mode == 3) {
                clearCells(0, 0, rows_ - 1, cols_ - 1);
            }
            break;
        }
        case 'K': {
            int mode = count > 0 ? params[0] : 0;
            if (mode == 0) clearCells(cur_row_, cur_col_, cur_row_, cols_ - 1);
            else if (mode == 1) clearCells(cur_row_, 0, cur_row_, cur_col_);
            else if (mode == 2) clearCells(cur_row_, 0, cur_row_, cols_ - 1);
            break;
        }
        case 'L':
            scrollDown(cur_row_, scroll_bottom_, p1(1));
            break;
        case 'M':
            scrollUp(cur_row_, scroll_bottom_, p1(1));
            break;
        case 'P': {
            int n = std::min(p1(1), cols_ - cur_col_);
            for (int c = cur_col_; c < cols_ - n; ++c) {
                cells_[cur_row_ * cols_ + c] = cells_[cur_row_ * cols_ + c + n];
            }
            clearCells(cur_row_, cols_ - n, cur_row_, cols_ - 1);
            break;
        }
        case '@': {
            int n = std::min(p1(1), cols_ - cur_col_);
            for (int c = cols_ - 1; c >= cur_col_ + n; --c) {
                cells_[cur_row_ * cols_ + c] = cells_[cur_row_ * cols_ + c - n];
            }
            clearCells(cur_row_, cur_col_, cur_row_, cur_col_ + n - 1);
            break;
        }
        case 'X': {
            int n = std::min(p1(1), cols_ - cur_col_);
            clearCells(cur_row_, cur_col_, cur_row_, cur_col_ + n - 1);
            break;
        }
        case 'm':
            handleSgr(params, count);
            break;
        case 'r': {
            int t = p1(1) - 1;
            int b = p2(rows_) - 1;
            if (t >= 0 && b < rows_ && t < b) {
                scroll_top_ = t;
                scroll_bottom_ = b;
            } else {
                scroll_top_ = 0;
                scroll_bottom_ = rows_ - 1;
            }
            cur_row_ = 0;
            cur_col_ = 0;
            pending_wrap_ = false;
            break;
        }
        case 's':
            saved_col_ = cur_col_;
            saved_row_ = cur_row_;
            break;
        case 'u':
            cur_col_ = std::min(cols_ - 1, saved_col_);
            cur_row_ = std::min(rows_ - 1, saved_row_);
            pending_wrap_ = false;
            break;
        default:
            break;
    }
}

void VT100Term::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = static_cast<uint8_t>(data[i]);

        if (state_ == State::OSC) {
            if (b == 0x07) state_ = State::TEXT;
            else if (b == 0x1B) state_ = State::OSC_ESC;
            continue;
        }
        if (state_ == State::OSC_ESC) {
            state_ = (b == '\\') ? State::TEXT : State::OSC;
            continue;
        }
        if (state_ == State::CHARSET) {
            state_ = State::TEXT;
            continue;
        }
        if (state_ == State::ESC) {
            if (b == '[') {
                state_ = State::CSI;
                csi_len_ = 0;
                csi_buf_[0] = '\0';
            } else if (b == ']') {
                state_ = State::OSC;
            } else if (b == '(' || b == ')') {
                state_ = State::CHARSET;
            } else if (b == 'M') {
                reverseIndex();
                state_ = State::TEXT;
            } else if (b == '7') {
                saved_col_ = cur_col_;
                saved_row_ = cur_row_;
                state_ = State::TEXT;
            } else if (b == '8') {
                cur_col_ = std::min(cols_ - 1, saved_col_);
                cur_row_ = std::min(rows_ - 1, saved_row_);
                pending_wrap_ = false;
                state_ = State::TEXT;
            } else if (b == 'c') {
                reset();
            } else {
                state_ = State::TEXT;
            }
            continue;
        }
        if (state_ == State::CSI) {
            if (b >= 0x40 && b <= 0x7E) {
                csi_buf_[csi_len_] = '\0';
                handleCsi(static_cast<char>(b));
                state_ = State::TEXT;
            } else if (csi_len_ < static_cast<int>(sizeof(csi_buf_) - 1)) {
                csi_buf_[csi_len_++] = static_cast<char>(b);
            }
            continue;
        }

        // State::TEXT
        if (utf8_rem_ > 0) {
            if ((b & 0xC0) == 0x80) {
                --utf8_rem_;
                continue;
            }
            utf8_rem_ = 0; // malformed sequence, fall through
        }

        if (b == 0x1B) {
            state_ = State::ESC;
        } else if (b == '\r') {
            cur_col_ = 0;
            pending_wrap_ = false;
        } else if (b == '\n' || b == 0x0B || b == 0x0C) {
            lineFeed();
        } else if (b == '\b') {
            pending_wrap_ = false;
            if (cur_col_ > 0) --cur_col_;
        } else if (b == '\t') {
            int next_tab = ((cur_col_ / 8) + 1) * 8;
            if (next_tab >= cols_) next_tab = cols_ - 1;
            while (cur_col_ < next_tab) {
                putChar(' ');
            }
        } else if (b == 0x07) {
            // BEL ignored
        } else if (b >= 0x20 && b < 0x7F) {
            putChar(static_cast<char>(b));
        } else if (b >= 0xC2 && b <= 0xDF) {
            utf8_rem_ = 1;
            putChar('?');
        } else if (b >= 0xE0 && b <= 0xEF) {
            utf8_rem_ = 2;
            putChar('?');
        } else if (b >= 0xF0 && b <= 0xF4) {
            utf8_rem_ = 3;
            putChar('?');
        }
    }
}
