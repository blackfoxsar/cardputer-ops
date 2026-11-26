#include "session_logger.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include <ctime>

#include "config.h"
#include "storage.h"
#include "time_sync.h"

SessionLogger::~SessionLogger() {
    stop();
}

bool SessionLogger::start(const HostProfile& profile) {
    stop();
    failed_ = false;
    error_.clear();
    path_.clear();
    buf_.clear();
    stripper_.reset();

    SPI.begin(cfg::SD_SCK, cfg::SD_MISO, cfg::SD_MOSI, cfg::SD_CS);
    spi_started_ = true;
    if (!SD.begin(cfg::SD_CS, SPI, 25000000)) {
        SPI.end();
        spi_started_ = false;
        failed_ = true;
        error_ = "SD card not detected";
        return false;
    }
    sd_mounted_ = true;

    if (!SD.exists(cfg::SD_DIR_SSH)) SD.mkdir(cfg::SD_DIR_SSH);
    if (!SD.exists(cfg::SD_DIR_LOGS)) SD.mkdir(cfg::SD_DIR_LOGS);

    std::string label = !profile.name.empty() ? profile.name : profile.host;
    std::string host_dir = std::string(cfg::SD_DIR_LOGS) + "/" + log_utils::sanitizeHostDir(label);
    if (!SD.exists(host_dir.c_str())) {
        SD.mkdir(host_dir.c_str());
    }

    time_t now = time(nullptr);
    std::string candidate;
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::string stem = time_sync::formatLogFilenameStem(now + attempt);
        candidate = host_dir + "/" + stem + ".log";
        if (!SD.exists(candidate.c_str())) break;
    }

    file_ = SD.open(candidate.c_str(), FILE_WRITE);
    if (!file_) {
        stop();
        failed_ = true;
        error_ = "Log open failed";
        return false;
    }

    path_ = candidate;
    AppSettings settings = storage::loadSettings();
    std::string hdr = "=== Cardputer Ops SSH Session ===\n"
                      "Host: " + profile.host + ":" + std::to_string(profile.port ? profile.port : 22) + "\n"
                      "User: " + profile.user + "\n"
                      "Started: " + (time_sync::isTimeSynced()
                                         ? (time_sync::formatDisplayTime() + " (" +
                                            time_sync::formatTzLabel(settings.tz_offset_hours) + ", " +
                                            time_sync::timeSourceLabel() + ")")
                                         : "UNSYNCED (set in Date & Time)") + "\n"
                      "============================\n";
    file_.write(reinterpret_cast<const uint8_t*>(hdr.data()), hdr.size());
    file_.flush();

    active_ = true;
    dirty_ = false;
    last_flush_ms_ = millis();
    return true;
}

bool SessionLogger::writeBuffered() {
    if (!active_ || buf_.empty()) return true;
    size_t written = file_.write(reinterpret_cast<const uint8_t*>(buf_.data()), buf_.size());
    buf_.clear();
    if (written == 0) {
        failed_ = true;
        error_ = "SD write error";
        return false;
    }
    dirty_ = true;
    return true;
}

void SessionLogger::appendRaw(const uint8_t* data, size_t len) {
    if (!active_ || !data || len == 0) return;
    std::string clean = stripper_.feed(reinterpret_cast<const char*>(data), len);
    if (clean.empty()) return;

    buf_ += clean;
    dirty_ = true;
    if (buf_.size() >= 512) {
        if (!writeBuffered()) {
            stop();
            return;
        }
        uint32_t now = millis();
        if (now - last_flush_ms_ >= 1500) {
            file_.flush();
            dirty_ = false;
            last_flush_ms_ = now;
        }
    }
}

void SessionLogger::pollFlush() {
    if (!active_) return;
    uint32_t now = millis();
    if ((dirty_ || !buf_.empty()) && (now - last_flush_ms_ >= 1200)) {
        if (!writeBuffered()) {
            stop();
            return;
        }
        file_.flush();
        dirty_ = false;
        last_flush_ms_ = now;
    }
}

void SessionLogger::stop() {
    if (active_) {
        writeBuffered();
        std::string ftr = "\n=== Session Ended: " +
                          (time_sync::isTimeSynced() ? time_sync::formatDisplayTime() : "UNSYNCED") +
                          " ===\n";
        file_.write(reinterpret_cast<const uint8_t*>(ftr.data()), ftr.size());
        file_.flush();
        file_.close();
        active_ = false;
        dirty_ = false;
    } else if (file_) {
        file_.close();
    }
    if (sd_mounted_) {
        SD.end();
        sd_mounted_ = false;
    }
    if (spi_started_) {
        SPI.end();
        spi_started_ = false;
    }
}
