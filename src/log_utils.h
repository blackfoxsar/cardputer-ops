#pragma once

#include <cstddef>
#include <string>

namespace log_utils {

std::string sanitizeHostDir(const std::string& raw);

class AnsiStripper {
public:
    AnsiStripper() = default;
    void reset();
    std::string feed(const char* data, size_t len);

private:
    enum class State {
        NORMAL,
        ESC,
        CSI,
        OSC,
        OSC_ESC,
        CHARSET,
    };

    State state_ = State::NORMAL;
};

}  // namespace log_utils
