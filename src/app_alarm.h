#pragma once

namespace app_alarm {

// Load persisted alarm configuration from LittleFS on boot.
void init();

// Suppress background alarm ringing during active audio capture/playback/SSH.
void setSuppressed(bool suppressed);
bool isSuppressed();

struct SuppressGuard {
    SuppressGuard() { setSuppressed(true); }
    ~SuppressGuard() { setSuppressed(false); }
    SuppressGuard(const SuppressGuard&) = delete;
    SuppressGuard& operator=(const SuppressGuard&) = delete;
};

// Called periodically from UI loop / pollInput; triggers alarm UI when due.
void checkBackground();

}  // namespace app_alarm

void runAlarmApp();
