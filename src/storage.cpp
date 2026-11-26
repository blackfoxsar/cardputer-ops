#include "storage.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

#include <algorithm>

#include "config.h"

namespace {

bool readWholeFile(fs::FS& fs, const char* path, std::string& out) {
    File f = fs.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        return false;
    }
    size_t sz = f.size();
    out.clear();
    out.reserve(sz);
    uint8_t buf[512];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        out.append(reinterpret_cast<const char*>(buf), n);
    }
    f.close();
    return true;
}

bool writeWholeFile(fs::FS& fs, const char* path, const std::string& data) {
    File f = fs.open(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    size_t written = 0;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
    size_t total = data.size();
    while (written < total) {
        size_t chunk = std::min<size_t>(512, total - written);
        size_t n = f.write(ptr + written, chunk);
        if (n == 0) {
            f.close();
            return false;
        }
        written += n;
    }
    f.close();
    return true;
}

bool copyFileBetweenFs(fs::FS& src_fs, const char* src_path,
                       fs::FS& dst_fs, const char* dst_path) {
    std::string data;
    if (!readWholeFile(src_fs, src_path, data)) {
        return false;
    }
    return writeWholeFile(dst_fs, dst_path, data);
}

std::string safeKeyBasename(const std::string& name) {
    std::string out;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        }
    }
    // Disallow path traversal
    if (out == "." || out == ".." || out.find("..") != std::string::npos) {
        return "";
    }
    return out;
}

std::string keyFullPath(const std::string& name) {
    std::string clean = safeKeyBasename(name);
    if (clean.empty()) return "";
    return std::string(cfg::PATH_KEYS_DIR) + "/" + clean;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void mergeBssidList(std::string& dst, const std::string& incoming) {
    if (incoming.empty()) return;
    auto splitTokens = [](const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= s.size()) {
            size_t pos = s.find(';', start);
            std::string tok = (pos == std::string::npos) ? s.substr(start) : s.substr(start, pos - start);
            if (!tok.empty()) out.push_back(tok);
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
        return out;
    };
    auto cur_tokens = splitTokens(dst);
    for (const auto& tok : splitTokens(incoming)) {
        if (std::find(cur_tokens.begin(), cur_tokens.end(), tok) == cur_tokens.end()) {
            if (cur_tokens.size() >= 8) break;
            cur_tokens.push_back(tok);
        }
    }
    dst.clear();
    for (size_t i = 0; i < cur_tokens.size(); ++i) {
        if (i > 0) dst.push_back(';');
        dst += cur_tokens[i];
    }
}

struct SdMountGuard {
    bool spi_started = false;
    bool mounted = false;

    bool mount() {
        SPI.begin(cfg::SD_SCK, cfg::SD_MISO, cfg::SD_MOSI, cfg::SD_CS);
        spi_started = true;
        mounted = SD.begin(cfg::SD_CS, SPI, 25000000);
        return mounted;
    }

    ~SdMountGuard() {
        if (mounted) {
            SD.end();
        }
        if (spi_started) {
            SPI.end();
        }
    }
};

}  // namespace

namespace storage {

bool begin() {
    if (!LittleFS.begin(true)) {
        return false;
    }
    if (!LittleFS.exists(cfg::PATH_KEYS_DIR)) {
        LittleFS.mkdir(cfg::PATH_KEYS_DIR);
    }
    return true;
}

FsStats getFsStats() {
    FsStats s;
    s.total_bytes = LittleFS.totalBytes();
    s.used_bytes = LittleFS.usedBytes();
    return s;
}

std::vector<HostProfile> loadHosts() {
    std::string raw;
    if (!readWholeFile(LittleFS, cfg::PATH_HOSTS_CSV, raw)) {
        return {};
    }
    return csv_codec::parseHostsCsv(raw);
}

bool saveHosts(const std::vector<HostProfile>& hosts) {
    return writeWholeFile(LittleFS, cfg::PATH_HOSTS_CSV, csv_codec::serializeHostsCsv(hosts));
}

std::vector<WifiEntry> loadWifi() {
    std::string raw;
    if (!readWholeFile(LittleFS, cfg::PATH_WIFI_CSV, raw)) {
        return {};
    }
    return csv_codec::parseWifiCsv(raw);
}

bool saveWifi(const std::vector<WifiEntry>& entries) {
    return writeWholeFile(LittleFS, cfg::PATH_WIFI_CSV, csv_codec::serializeWifiCsv(entries));
}

std::vector<KnownHostEntry> loadKnownHosts() {
    std::string raw;
    if (!readWholeFile(LittleFS, cfg::PATH_KNOWN_CSV, raw)) {
        return {};
    }
    return csv_codec::parseKnownHostsCsv(raw);
}

bool saveKnownHosts(const std::vector<KnownHostEntry>& entries) {
    return writeWholeFile(LittleFS, cfg::PATH_KNOWN_CSV, csv_codec::serializeKnownHostsCsv(entries));
}

std::string findKnownHostFingerprint(const std::string& host, uint16_t port) {
    for (const auto& entry : loadKnownHosts()) {
        if (entry.host == host && entry.port == port) {
            return entry.fingerprint;
        }
    }
    return "";
}

bool upsertKnownHost(const std::string& host, uint16_t port, const std::string& fp) {
    auto list = loadKnownHosts();
    for (auto& entry : list) {
        if (entry.host == host && entry.port == port) {
            entry.fingerprint = fp;
            return saveKnownHosts(list);
        }
    }
    list.push_back(KnownHostEntry{host, port, fp});
    return saveKnownHosts(list);
}

bool clearKnownHosts() {
    if (LittleFS.exists(cfg::PATH_KNOWN_CSV)) {
        return LittleFS.remove(cfg::PATH_KNOWN_CSV);
    }
    return true;
}

AppSettings loadSettings() {
    AppSettings s;
    std::string raw;
    if (!readWholeFile(LittleFS, cfg::PATH_SETTINGS, raw)) {
        return s;
    }
    size_t start = 0;
    while (start <= raw.size()) {
        size_t nl = raw.find('\n', start);
        size_t end = (nl == std::string::npos) ? raw.size() : nl;
        std::string line = raw.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            if (k == "default_legacy") {
                s.default_legacy = (v == "1" || v == "true");
            } else if (k == "sd_log_enabled") {
                s.sd_log_enabled = (v == "1" || v == "true");
            } else if (k == "tz_offset_hours") {
                int tz = std::atoi(v.c_str());
                if (tz < -12) tz = -12;
                if (tz > 14) tz = 14;
                s.tz_offset_hours = tz;
            } else if (k == "ntp_server" && !v.empty()) {
                s.ntp_server = v;
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return s;
}

bool saveSettings(const AppSettings& s) {
    int tz = s.tz_offset_hours;
    if (tz < -12) tz = -12;
    if (tz > 14) tz = 14;
    std::string srv = s.ntp_server.empty() ? "pool.ntp.org" : s.ntp_server;
    std::string out = "default_legacy=" + std::string(s.default_legacy ? "1" : "0") + "\n" +
                      "sd_log_enabled=" + std::string(s.sd_log_enabled ? "1" : "0") + "\n" +
                      "tz_offset_hours=" + std::to_string(tz) + "\n" +
                      "ntp_server=" + srv + "\n";
    return writeWholeFile(LittleFS, cfg::PATH_SETTINGS, out);
}

bool loadDefaultLegacy() {
    return loadSettings().default_legacy;
}

bool saveDefaultLegacy(bool legacy) {
    AppSettings s = loadSettings();
    s.default_legacy = legacy;
    return saveSettings(s);
}

std::vector<std::string> listKeys(bool include_pub) {
    std::vector<std::string> names;
    File dir = LittleFS.open(cfg::PATH_KEYS_DIR);
    if (!dir || !dir.isDirectory()) {
        return names;
    }
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            std::string name = entry.name();
            size_t slash = name.find_last_of('/');
            if (slash != std::string::npos) {
                name = name.substr(slash + 1);
            }
            if (!name.empty() && name[0] != '.' && (include_pub || !endsWith(name, ".pub"))) {
                names.push_back(name);
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    std::sort(names.begin(), names.end());
    return names;
}

bool readKeyFile(const std::string& filename, std::string& out_data) {
    std::string path = keyFullPath(filename);
    if (path.empty()) return false;
    return readWholeFile(LittleFS, path.c_str(), out_data);
}

bool writeKeyFile(const std::string& filename, const std::string& data) {
    if (!LittleFS.exists(cfg::PATH_KEYS_DIR)) {
        LittleFS.mkdir(cfg::PATH_KEYS_DIR);
    }
    std::string path = keyFullPath(filename);
    if (path.empty()) return false;
    return writeWholeFile(LittleFS, path.c_str(), data);
}

bool deleteKeyPair(const std::string& priv_name) {
    std::string p = keyFullPath(priv_name);
    if (p.empty()) return false;
    bool ok = true;
    if (LittleFS.exists(p.c_str())) {
        ok = LittleFS.remove(p.c_str());
    }
    std::string pub = p + ".pub";
    if (LittleFS.exists(pub.c_str())) {
        LittleFS.remove(pub.c_str());
    }
    return ok;
}

SyncReport importFromSD() {
    SyncReport rep;
    SdMountGuard sd;
    if (!sd.mount()) {
        rep.error = "SD card not detected";
        return rep;
    }
    rep.sd_ok = true;

    // 1. Hosts CSV merge
    std::string raw_hosts;
    if (readWholeFile(SD, cfg::SD_HOSTS_CSV, raw_hosts)) {
        auto incoming = csv_codec::parseHostsCsv(raw_hosts);
        auto current = loadHosts();
        for (const auto& inc : incoming) {
            bool updated = false;
            for (auto& cur : current) {
                if (cur.host == inc.host && cur.port == inc.port && cur.user == inc.user) {
                    cur = inc;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                current.push_back(inc);
            }
            ++rep.hosts_count;
        }
        if (!incoming.empty()) {
            saveHosts(current);
        }
    }

    // 2. WiFi CSV merge
    std::string raw_wifi;
    if (readWholeFile(SD, cfg::SD_WIFI_CSV, raw_wifi)) {
        auto incoming = csv_codec::parseWifiCsv(raw_wifi);
        auto current = loadWifi();
        for (const auto& inc : incoming) {
            bool updated = false;
            for (auto& cur : current) {
                if (cur.ssid == inc.ssid) {
                    cur.password = inc.password;
                    cur.hidden = cur.hidden || inc.hidden;
                    mergeBssidList(cur.bssid, inc.bssid);
                    if (!inc.use_dhcp || !inc.static_ip.empty()) {
                        cur.use_dhcp = inc.use_dhcp;
                        cur.static_ip = inc.static_ip;
                        cur.subnet = inc.subnet;
                        cur.gateway = inc.gateway;
                        cur.dns = inc.dns;
                    }
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                current.push_back(inc);
            }
            ++rep.wifi_count;
        }
        if (!incoming.empty()) {
            saveWifi(current);
        }
    }

    // 3. Keys copy
    if (SD.exists(cfg::SD_DIR_KEYS)) {
        File dir = SD.open(cfg::SD_DIR_KEYS);
        if (dir && dir.isDirectory()) {
            File entry = dir.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    std::string name = entry.name();
                    size_t slash = name.find_last_of('/');
                    if (slash != std::string::npos) {
                        name = name.substr(slash + 1);
                    }
                    std::string clean = safeKeyBasename(name);
                    if (!clean.empty() && clean[0] != '.') {
                        std::string src_path = std::string(cfg::SD_DIR_KEYS) + "/" + clean;
                        std::string dst_path = std::string(cfg::PATH_KEYS_DIR) + "/" + clean;
                        entry.close();
                        if (copyFileBetweenFs(SD, src_path.c_str(), LittleFS, dst_path.c_str())) {
                            ++rep.keys_count;
                        }
                        entry = dir.openNextFile();
                        continue;
                    }
                }
                entry.close();
                entry = dir.openNextFile();
            }
            dir.close();
        }
    }

    return rep;
}

SyncReport exportToSD() {
    SyncReport rep;
    SdMountGuard sd;
    if (!sd.mount()) {
        rep.error = "SD card not detected";
        return rep;
    }
    rep.sd_ok = true;

    if (!SD.exists(cfg::SD_DIR_SSH)) {
        SD.mkdir(cfg::SD_DIR_SSH);
    }
    if (!SD.exists(cfg::SD_DIR_KEYS)) {
        SD.mkdir(cfg::SD_DIR_KEYS);
    }

    auto hosts = loadHosts();
    if (writeWholeFile(SD, cfg::SD_HOSTS_CSV, csv_codec::serializeHostsCsv(hosts))) {
        rep.hosts_count = static_cast<int>(hosts.size());
    }

    auto wifi = loadWifi();
    if (writeWholeFile(SD, cfg::SD_WIFI_CSV, csv_codec::serializeWifiCsv(wifi))) {
        rep.wifi_count = static_cast<int>(wifi.size());
    }

    for (const auto& name : listKeys(true)) {
        std::string src = std::string(cfg::PATH_KEYS_DIR) + "/" + name;
        std::string dst = std::string(cfg::SD_DIR_KEYS) + "/" + name;
        if (copyFileBetweenFs(LittleFS, src.c_str(), SD, dst.c_str())) {
            ++rep.keys_count;
        }
    }

    return rep;
}

KeySdResult exportKeyToSD(const std::string& priv_name, bool export_priv, bool export_pub) {
    KeySdResult res;
    std::string clean = safeKeyBasename(priv_name);
    if (clean.empty() || clean[0] == '.') {
        res.error = "Invalid key filename";
        return res;
    }

    SdMountGuard sd;
    if (!sd.mount()) {
        res.error = "SD card not detected";
        return res;
    }
    res.sd_ok = true;

    if (!SD.exists(cfg::SD_DIR_SSH)) {
        SD.mkdir(cfg::SD_DIR_SSH);
    }
    if (!SD.exists(cfg::SD_DIR_KEYS)) {
        SD.mkdir(cfg::SD_DIR_KEYS);
    }

    if (export_priv) {
        std::string src = std::string(cfg::PATH_KEYS_DIR) + "/" + clean;
        std::string dst = std::string(cfg::SD_DIR_KEYS) + "/" + clean;
        if (copyFileBetweenFs(LittleFS, src.c_str(), SD, dst.c_str())) {
            res.did_priv = true;
        }
    }
    if (export_pub) {
        std::string src_pub = std::string(cfg::PATH_KEYS_DIR) + "/" + clean + ".pub";
        if (LittleFS.exists(src_pub.c_str())) {
            std::string dst_pub = std::string(cfg::SD_DIR_KEYS) + "/" + clean + ".pub";
            if (copyFileBetweenFs(LittleFS, src_pub.c_str(), SD, dst_pub.c_str())) {
                res.did_pub = true;
            }
        }
    }

    res.ok = (res.did_priv || res.did_pub);
    if (!res.ok) {
        res.error = "Key file write failed";
    }
    return res;
}

std::vector<std::string> listSdKeys(bool& sd_ok, std::string& err) {
    sd_ok = false;
    err.clear();
    std::vector<std::string> names;

    SdMountGuard sd;
    if (!sd.mount()) {
        err = "SD card not detected";
        return names;
    }
    sd_ok = true;

    if (!SD.exists(cfg::SD_DIR_KEYS)) {
        return names;
    }

    File dir = SD.open(cfg::SD_DIR_KEYS);
    if (!dir || !dir.isDirectory()) {
        return names;
    }
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            std::string name = entry.name();
            size_t slash = name.find_last_of('/');
            if (slash != std::string::npos) {
                name = name.substr(slash + 1);
            }
            std::string clean = safeKeyBasename(name);
            if (!clean.empty() && clean[0] != '.' && !endsWith(clean, ".pub")) {
                names.push_back(clean);
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    std::sort(names.begin(), names.end());
    return names;
}

KeySdResult importKeyFromSD(const std::string& priv_name) {
    KeySdResult res;
    std::string clean = safeKeyBasename(priv_name);
    if (clean.empty() || clean[0] == '.') {
        res.error = "Invalid key filename";
        return res;
    }

    SdMountGuard sd;
    if (!sd.mount()) {
        res.error = "SD card not detected";
        return res;
    }
    res.sd_ok = true;

    if (!LittleFS.exists(cfg::PATH_KEYS_DIR)) {
        LittleFS.mkdir(cfg::PATH_KEYS_DIR);
    }

    std::string src = std::string(cfg::SD_DIR_KEYS) + "/" + clean;
    std::string dst = std::string(cfg::PATH_KEYS_DIR) + "/" + clean;
    if (copyFileBetweenFs(SD, src.c_str(), LittleFS, dst.c_str())) {
        res.did_priv = true;
    }

    std::string src_pub = src + ".pub";
    if (SD.exists(src_pub.c_str())) {
        std::string dst_pub = dst + ".pub";
        if (copyFileBetweenFs(SD, src_pub.c_str(), LittleFS, dst_pub.c_str())) {
            res.did_pub = true;
        }
    }

    res.ok = res.did_priv;
    if (!res.ok) {
        res.error = "Copy from SD failed";
    }
    return res;
}

}  // namespace storage
