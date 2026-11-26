#include "csv_codec.h"

#include <cctype>
#include <cstdlib>

namespace {

std::string trimWhitespace(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool parseBoolFlag(const std::string& val) {
    std::string low = toLowerAscii(trimWhitespace(val));
    return low == "1" || low == "true" || low == "yes" || low == "on" || low == "y";
}

uint16_t parsePort(const std::string& val) {
    std::string t = trimWhitespace(val);
    if (t.empty()) {
        return 22;
    }
    char* endp = nullptr;
    long v = std::strtol(t.c_str(), &endp, 10);
    if (endp == t.c_str() || *endp != '\0' || v < 1 || v > 65535) {
        return 22;
    }
    return static_cast<uint16_t>(v);
}

std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::string cur;
    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];
        if (c == '\r') {
            if (i + 1 < content.size() && content[i + 1] == '\n') {
                ++i;
            }
            lines.push_back(cur);
            cur.clear();
        } else if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        lines.push_back(cur);
    }
    return lines;
}

bool isCommentOrEmpty(const std::string& raw) {
    std::string t = trimWhitespace(raw);
    return t.empty() || t[0] == '#';
}

}  // namespace

std::string HostProfile::displayLabel() const {
    if (!name.empty()) {
        return name;
    }
    std::string out = user.empty() ? host : (user + "@" + host);
    if (port != 22) {
        out += ":" + std::to_string(port);
    }
    return out;
}

namespace csv_codec {

std::vector<std::string> parseCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool in_quotes = false;
    bool at_field_start = true;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                cur.push_back(c);
            }
        } else {
            if (at_field_start && c == '"') {
                in_quotes = true;
                at_field_start = false;
            } else if (c == ',') {
                fields.push_back(cur);
                cur.clear();
                at_field_start = true;
            } else if (c != '\r') {
                cur.push_back(c);
                at_field_start = false;
            }
        }
    }
    fields.push_back(cur);
    return fields;
}

std::string escapeCsvField(const std::string& field) {
    bool needs_quote = false;
    for (char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) {
        return field;
    }
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::vector<HostProfile> parseHostsCsv(const std::string& content) {
    std::vector<HostProfile> hosts;
    bool header_checked = false;

    for (const std::string& raw : splitLines(content)) {
        if (isCommentOrEmpty(raw)) {
            continue;
        }
        auto cols = parseCsvLine(raw);
        if (cols.empty()) {
            continue;
        }
        std::string c0 = trimWhitespace(cols[0]);
        if (!header_checked) {
            header_checked = true;
            std::string low0 = toLowerAscii(c0);
            std::string low1 = cols.size() > 1 ? toLowerAscii(trimWhitespace(cols[1])) : "";
            if ((low0 == "host" || low0 == "ip" || low0 == "ip/domain") &&
                (low1 == "user" || low1 == "account" || low1 == "username")) {
                continue;
            }
        }
        if (c0.empty()) {
            continue;
        }

        HostProfile p;
        p.host = c0;
        if (cols.size() > 1) p.user = trimWhitespace(cols[1]);
        if (cols.size() > 2) p.password = cols[2];
        if (cols.size() > 3) p.port = parsePort(cols[3]);
        if (cols.size() > 4) p.key_file = trimWhitespace(cols[4]);
        if (cols.size() > 5) p.legacy = parseBoolFlag(cols[5]);
        if (cols.size() > 6) p.name = trimWhitespace(cols[6]);
        hosts.push_back(p);
    }
    return hosts;
}

std::string serializeHostsCsv(const std::vector<HostProfile>& hosts) {
    std::string out = "host,user,password,port,key_file,legacy,name\n";
    for (const auto& h : hosts) {
        out += escapeCsvField(h.host) + "," +
               escapeCsvField(h.user) + "," +
               escapeCsvField(h.password) + "," +
               std::to_string(h.port ? h.port : 22) + "," +
               escapeCsvField(h.key_file) + "," +
               (h.legacy ? "1" : "0") + "," +
               escapeCsvField(h.name) + "\n";
    }
    return out;
}

std::vector<WifiEntry> parseWifiCsv(const std::string& content) {
    std::vector<WifiEntry> list;
    bool header_checked = false;

    for (const std::string& raw : splitLines(content)) {
        if (isCommentOrEmpty(raw)) {
            continue;
        }
        auto cols = parseCsvLine(raw);
        if (cols.empty()) {
            continue;
        }
        std::string ssid = trimWhitespace(cols[0]);
        if (!header_checked) {
            header_checked = true;
            std::string low0 = toLowerAscii(ssid);
            if (low0 == "ssid" || low0 == "network") {
                continue;
            }
        }
        if (ssid.empty()) {
            continue;
        }
        WifiEntry w;
        w.ssid = ssid;
        if (cols.size() > 1) {
            w.password = cols[1];
        }
        if (cols.size() > 2) {
            w.hidden = parseBoolFlag(cols[2]);
        }
        if (cols.size() > 3) {
            w.bssid = toLowerAscii(trimWhitespace(cols[3]));
        }
        if (cols.size() > 4) {
            std::string d = trimWhitespace(cols[4]);
            if (!d.empty()) {
                w.use_dhcp = parseBoolFlag(d);
            }
        }
        if (cols.size() > 5) {
            w.static_ip = trimWhitespace(cols[5]);
        }
        if (cols.size() > 6) {
            w.subnet = trimWhitespace(cols[6]);
        }
        if (cols.size() > 7) {
            w.gateway = trimWhitespace(cols[7]);
        }
        if (cols.size() > 8) {
            w.dns = trimWhitespace(cols[8]);
        }
        list.push_back(w);
    }
    return list;
}

std::string serializeWifiCsv(const std::vector<WifiEntry>& entries) {
    std::string out = "ssid,password,hidden,bssid,dhcp,ip,mask,gw,dns\n";
    for (const auto& w : entries) {
        out += escapeCsvField(w.ssid) + "," +
               escapeCsvField(w.password) + "," +
               (w.hidden ? "1" : "0") + "," +
               escapeCsvField(w.bssid) + "," +
               (w.use_dhcp ? "1" : "0") + "," +
               escapeCsvField(w.static_ip) + "," +
               escapeCsvField(w.subnet) + "," +
               escapeCsvField(w.gateway) + "," +
               escapeCsvField(w.dns) + "\n";
    }
    return out;
}

std::vector<KnownHostEntry> parseKnownHostsCsv(const std::string& content) {
    std::vector<KnownHostEntry> list;
    bool header_checked = false;

    for (const std::string& raw : splitLines(content)) {
        if (isCommentOrEmpty(raw)) {
            continue;
        }
        auto cols = parseCsvLine(raw);
        if (cols.size() < 3) {
            continue;
        }
        std::string host = trimWhitespace(cols[0]);
        if (!header_checked) {
            header_checked = true;
            if (toLowerAscii(host) == "host") {
                continue;
            }
        }
        if (host.empty()) {
            continue;
        }
        KnownHostEntry k;
        k.host = host;
        k.port = parsePort(cols[1]);
        k.fingerprint = trimWhitespace(cols[2]);
        list.push_back(k);
    }
    return list;
}

std::string serializeKnownHostsCsv(const std::vector<KnownHostEntry>& entries) {
    std::string out = "host,port,fingerprint\n";
    for (const auto& k : entries) {
        out += escapeCsvField(k.host) + "," +
               std::to_string(k.port ? k.port : 22) + "," +
               escapeCsvField(k.fingerprint) + "\n";
    }
    return out;
}

}  // namespace csv_codec
