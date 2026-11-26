#pragma once

#include <string>
#include <vector>

#include "csv_codec.h"

struct SyncReport {
    bool sd_ok = false;
    std::string error;
    int hosts_count = 0;
    int wifi_count = 0;
    int keys_count = 0;
};

struct KeySdResult {
    bool sd_ok = false;
    bool ok = false;
    std::string error;
    bool did_priv = false;
    bool did_pub = false;
};

struct FsStats {
    size_t total_bytes = 0;
    size_t used_bytes = 0;
};

struct AppSettings {
    bool default_legacy = false;
    bool sd_log_enabled = false;
    int tz_offset_hours = 9;
    std::string ntp_server = "pool.ntp.org";
};

namespace storage {

bool begin();
FsStats getFsStats();

AppSettings loadSettings();
bool saveSettings(const AppSettings& settings);

std::vector<HostProfile> loadHosts();
bool saveHosts(const std::vector<HostProfile>& hosts);

std::vector<WifiEntry> loadWifi();
bool saveWifi(const std::vector<WifiEntry>& entries);

std::vector<KnownHostEntry> loadKnownHosts();
bool saveKnownHosts(const std::vector<KnownHostEntry>& entries);
std::string findKnownHostFingerprint(const std::string& host, uint16_t port);
bool upsertKnownHost(const std::string& host, uint16_t port, const std::string& fp);
bool clearKnownHosts();

bool loadDefaultLegacy();
bool saveDefaultLegacy(bool legacy);

std::vector<std::string> listKeys(bool include_pub = false);
bool readKeyFile(const std::string& filename, std::string& out_data);
bool writeKeyFile(const std::string& filename, const std::string& data);
bool deleteKeyPair(const std::string& priv_name);

KeySdResult exportKeyToSD(const std::string& priv_name, bool export_priv, bool export_pub);
std::vector<std::string> listSdKeys(bool& sd_ok, std::string& err);
KeySdResult importKeyFromSD(const std::string& priv_name);

SyncReport importFromSD();
SyncReport exportToSD();

}  // namespace storage
