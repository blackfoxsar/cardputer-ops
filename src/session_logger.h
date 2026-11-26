#pragma once

#include <FS.h>
#include <stdint.h>
#include <string>

#include "csv_codec.h"
#include "log_utils.h"

class SessionLogger {
public:
    SessionLogger() = default;
    ~SessionLogger();

    SessionLogger(const SessionLogger&) = delete;
    SessionLogger& operator=(const SessionLogger&) = delete;

    bool start(const HostProfile& profile);
    void appendRaw(const uint8_t* data, size_t len);
    void pollFlush();
    void stop();

    bool active() const { return active_; }
    bool failed() const { return failed_; }
    const std::string& path() const { return path_; }
    const std::string& error() const { return error_; }

private:
    bool writeBuffered();

    bool sd_mounted_ = false;
    bool spi_started_ = false;
    bool active_ = false;
    bool failed_ = false;
    bool dirty_ = false;
    File file_;
    std::string path_;
    std::string error_;
    std::string buf_;
    log_utils::AnsiStripper stripper_;
    uint32_t last_flush_ms_ = 0;
};
