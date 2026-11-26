#include "log_utils.h"

namespace log_utils {

std::string sanitizeHostDir(const std::string& raw) {
    std::string out;
    out.reserve(32);
    bool last_underscore = false;
    for (char c : raw) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        char mapped = ok ? c : '_';
        if (mapped == '_') {
            if (last_underscore) continue;
            last_underscore = true;
        } else {
            last_underscore = false;
        }
        out.push_back(mapped);
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '_')) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) {
        out.pop_back();
    }
    if (out.size() > 32) {
        out.resize(32);
        while (!out.empty() && (out.back() == '.' || out.back() == '_')) {
            out.pop_back();
        }
    }
    if (out.empty()) {
        return "host";
    }
    return out;
}

void AnsiStripper::reset() {
    state_ = State::NORMAL;
}

std::string AnsiStripper::feed(const char* data, size_t len) {
    std::string out;
    if (!data || len == 0) return out;
    out.reserve(len);

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        switch (state_) {
            case State::NORMAL:
                if (c == 0x1B) {
                    state_ = State::ESC;
                } else if (c == '\r') {
                    // Drop CR so logs have clean LF line endings on all OSes
                } else if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F) || c >= 0x80) {
                    out.push_back(static_cast<char>(c));
                }
                break;

            case State::ESC:
                if (c == '[') {
                    state_ = State::CSI;
                } else if (c == ']') {
                    state_ = State::OSC;
                } else if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#') {
                    state_ = State::CHARSET;
                } else {
                    state_ = State::NORMAL;
                }
                break;

            case State::CSI:
                if (c >= 0x40 && c <= 0x7E) {
                    state_ = State::NORMAL;
                }
                break;

            case State::OSC:
                if (c == 0x07) {
                    state_ = State::NORMAL;
                } else if (c == 0x1B) {
                    state_ = State::OSC_ESC;
                }
                break;

            case State::OSC_ESC:
                if (c == '\\') {
                    state_ = State::NORMAL;
                } else if (c != 0x1B) {
                    state_ = State::OSC;
                }
                break;

            case State::CHARSET:
                state_ = State::NORMAL;
                break;
        }
    }
    return out;
}

}  // namespace log_utils
