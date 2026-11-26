#pragma once

#include <stdint.h>
#include <string>
#include <vector>

struct HostProfile {
    std::string host;
    std::string user;
    std::string password;
    uint16_t port = 22;
    std::string key_file;
    bool legacy = false;
    std::string name;

    std::string displayLabel() const;
};

struct WifiEntry {
    std::string ssid;
    std::string password;
    bool hidden = false;
    std::string bssid;     // semicolon-separated lowercase MACs (e.g. "aa:bb:cc:dd:ee:ff")
    bool use_dhcp = true;
    std::string static_ip;
    std::string subnet;
    std::string gateway;
    std::string dns;
};

struct KnownHostEntry {
    std::string host;
    uint16_t port = 22;
    std::string fingerprint;
};

namespace csv_codec {

std::vector<std::string> parseCsvLine(const std::string& line);
std::string escapeCsvField(const std::string& field);

std::vector<HostProfile> parseHostsCsv(const std::string& content);
std::string serializeHostsCsv(const std::vector<HostProfile>& hosts);

std::vector<WifiEntry> parseWifiCsv(const std::string& content);
std::string serializeWifiCsv(const std::vector<WifiEntry>& entries);

std::vector<KnownHostEntry> parseKnownHostsCsv(const std::string& content);
std::string serializeKnownHostsCsv(const std::vector<KnownHostEntry>& entries);

}  // namespace csv_codec
