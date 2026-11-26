#include "app_wifi.h"

#include <WiFi.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <sys/select.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "config.h"
#include "storage.h"
#include "time_sync.h"
#include "ui_common.h"

namespace {

struct ApRow {
    std::string ssid;
    std::string bssid;
    std::string display;
    int32_t rssi = -99;
    std::string auth;
    bool open = false;
    bool hidden = false;
    bool known_hidden = false;
};

struct IpPlan {
    bool use_dhcp = true;
    std::string ip;
    std::string mask = "255.255.255.0";
    std::string gw;
    std::string dns = "8.8.8.8";
};

bool g_wifi_events_init = false;
volatile bool g_sta_l2_connected = false;
volatile bool g_sta_got_ip = false;
bool g_conn_hidden = false;
bool g_conn_static = false;

void ensureWifiEvents() {
    if (g_wifi_events_init) return;
    g_wifi_events_init = true;
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t) {
        if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
            g_sta_l2_connected = true;
        } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            g_sta_got_ip = true;
        } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            g_sta_l2_connected = false;
            g_sta_got_ip = false;
        }
    });
}

std::string formatMacLower(const uint8_t* mac) {
    if (!mac) return "";
    char buf[20];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool bssidListContains(const std::string& list, const std::string& mac) {
    if (list.empty() || mac.empty()) return false;
    size_t start = 0;
    while (start <= list.size()) {
        size_t pos = list.find(';', start);
        std::string tok = (pos == std::string::npos) ? list.substr(start) : list.substr(start, pos - start);
        if (!tok.empty() && equalsIgnoreCase(tok, mac)) return true;
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return false;
}

void appendBssid(std::string& list, const std::string& raw_mac) {
    if (raw_mac.empty()) return;
    std::string mac = raw_mac;
    for (char& c : mac) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (bssidListContains(list, mac)) return;
    int count = 0;
    size_t start = 0;
    while (!list.empty() && start <= list.size()) {
        size_t pos = list.find(';', start);
        if (pos == std::string::npos) {
            ++count;
            break;
        }
        ++count;
        start = pos + 1;
    }
    if (count >= 8) return;
    if (!list.empty()) list.push_back(';');
    list += mac;
}

const char* authModeName(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA1/2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
        default: return "?";
    }
}

uint16_t signalColor(int32_t rssi) {
    if (rssi >= -55) return cfg::COL_GREEN;
    if (rssi >= -68) return cfg::COL_CYAN;
    if (rssi >= -78) return cfg::COL_CREAM;
    if (rssi >= -86) return cfg::COL_YELLOW;
    return cfg::COL_RED;
}

std::vector<ApRow> scanNetworksUi() {
    auto& cv = ui::canvas();
    cv.fillScreen(cfg::COL_BG);
    ui::drawHeader("Wi-Fi");
    cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
    cv.drawString("Scanning 2.4GHz networks...", 24, 58);
    ui::drawHints("Please wait");
    cv.pushSprite(0, 0);

    ensureWifiEvents();
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, true);
    auto saved = storage::loadWifi();

    std::vector<ApRow> list;
    for (int i = 0; i < n; ++i) {
        std::string ssid = WiFi.SSID(i).c_str();
        std::string bssid = formatMacLower(WiFi.BSSID(i));
        bool is_hidden = ssid.empty();
        bool known_hidden = false;
        std::string display;
        if (is_hidden) {
            for (const auto& w : saved) {
                if (w.hidden && !w.ssid.empty() && bssidListContains(w.bssid, bssid)) {
                    ssid = w.ssid;
                    display = "*" + w.ssid;
                    known_hidden = true;
                    break;
                }
            }
            if (!known_hidden) {
                display = bssid.empty() ? "<hidden>" : ("<" + bssid + ">");
            }
        } else {
            display = ssid;
        }
        int32_t rssi = WiFi.RSSI(i);
        wifi_auth_mode_t am = WiFi.encryptionType(i);
        bool is_open = (am == WIFI_AUTH_OPEN);
        std::string auth = authModeName(am);

        bool merged = false;
        for (auto& existing : list) {
            bool same = false;
            if (!is_hidden) {
                same = (!existing.hidden && existing.ssid == ssid);
            } else if (known_hidden) {
                same = (existing.hidden && existing.known_hidden && existing.ssid == ssid);
            } else {
                same = (existing.hidden && !existing.known_hidden && !bssid.empty() && existing.bssid == bssid);
            }
            if (same) {
                if (rssi > existing.rssi) {
                    existing.rssi = rssi;
                    existing.auth = auth;
                    existing.open = is_open;
                    existing.bssid = bssid;
                    existing.display = display;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            list.push_back(ApRow{ssid, bssid, display, rssi, auth, is_open, is_hidden, known_hidden});
        }
    }
    WiFi.scanDelete();
    std::sort(list.begin(), list.end(), [](const ApRow& a, const ApRow& b) {
        return a.rssi > b.rssi;
    });
    return list;
}

bool isValidIpv4Literal(const std::string& s) {
    int dots = 0;
    int part = 0;
    int digits = 0;
    for (char c : s) {
        if (c == '.') {
            if (digits == 0 || part > 255) return false;
            ++dots;
            part = 0;
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            part = part * 10 + (c - '0');
            if (++digits > 3 || part > 255) return false;
        } else {
            return false;
        }
    }
    return dots == 3 && digits > 0 && part <= 255;
}

void parseHostPort(const std::string& raw, std::string& host, uint16_t& port) {
    port = 80;
    size_t colon = raw.rfind(':');
    if (colon != std::string::npos && raw.find(':') == colon && colon > 0) {
        std::string pstr = raw.substr(colon + 1);
        bool all_dig = !pstr.empty();
        for (char c : pstr) {
            if (c < '0' || c > '9') {
                all_dig = false;
                break;
            }
        }
        if (all_dig) {
            int pv = std::atoi(pstr.c_str());
            if (pv >= 1 && pv <= 65535) {
                host = raw.substr(0, colon);
                port = static_cast<uint16_t>(pv);
                return;
            }
        }
    }
    host = raw;
}

struct ProbeResult {
    bool ok = false;
    bool rst = false;
    float rtt_ms = 0.0f;
};

ProbeResult tcpPingOnce(const IPAddress& ip, uint16_t port, uint32_t timeout_ms) {
    ProbeResult res;
    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return res;

    int flags = lwip_fcntl(sock, F_GETFL, 0);
    lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = static_cast<uint32_t>(ip);

    uint64_t t0 = esp_timer_get_time();
    int rc = lwip_connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        res.ok = true;
        res.rtt_ms = static_cast<float>(esp_timer_get_time() - t0) / 1000.0f;
        lwip_close(sock);
        return res;
    }

    if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = lwip_select(sock + 1, nullptr, &wfds, nullptr, &tv);
        if (sel > 0) {
            int so_err = 0;
            socklen_t len = sizeof(so_err);
            lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len);
            float dt = static_cast<float>(esp_timer_get_time() - t0) / 1000.0f;
            if (so_err == 0) {
                res.ok = true;
                res.rtt_ms = dt;
            } else if (so_err == ECONNREFUSED) {
                res.ok = true;
                res.rst = true;
                res.rtt_ms = dt;
            }
        }
    }
    lwip_close(sock);
    return res;
}

void runPingScreen() {
    std::string target;
    if (!ui::promptText("TCP Ping", "Enter host or host:port", target, false, 63) || target.empty()) {
        return;
    }

    std::string host;
    uint16_t port = 80;
    parseHostPort(target, host, port);

    IPAddress ip;
    bool resolved = false;
    if (isValidIpv4Literal(host)) {
        resolved = ip.fromString(host.c_str());
    } else {
        resolved = (WiFi.hostByName(host.c_str(), ip) == 1);
    }

    if (!resolved) {
        ui::showToast("Ping Failed", "DNS resolve failed:", host, cfg::COL_RED);
        return;
    }

    std::vector<std::string> lines;
    std::vector<uint16_t> colors;
    lines.push_back("Target: " + std::string(ip.toString().c_str()) + ":" + std::to_string(port));
    colors.push_back(cfg::COL_GRAY);

    std::vector<float> rtts;
    bool stopped = false;
    ui::waitKeyRelease();

    for (int seq = 1; seq <= 4; ++seq) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        std::string hdr = "Ping " + ui::fitCols(host, 18);
        ui::drawHeader(hdr.c_str());
        for (size_t i = 0; i < lines.size(); ++i) {
            cv.setTextColor(colors[i], cfg::COL_BG);
            cv.drawString(lines[i].c_str(), 10, 26 + static_cast<int>(i) * 15);
        }
        std::string h = "Probe " + std::to_string(seq) + "/4...";
        ui::drawHints(h.c_str(), "Q / G0 stop");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            stopped = true;
            break;
        }

        ProbeResult pr = tcpPingOnce(ip, port, 2500);
        if (pr.ok) {
            rtts.push_back(pr.rtt_ms);
            char buf[48];
            snprintf(buf, sizeof(buf), "seq=%d  %.1f ms%s", seq, pr.rtt_ms, pr.rst ? " (rst)" : "");
            lines.push_back(buf);
            colors.push_back(cfg::COL_GREEN);
        } else {
            lines.push_back("seq=" + std::to_string(seq) + "  timeout/lost");
            colors.push_back(cfg::COL_RED);
        }
    }

    int sent = stopped ? static_cast<int>(lines.size() - 1) : 4;
    std::string sum;
    if (!rtts.empty()) {
        float mn = rtts[0], mx = rtts[0], tot = 0.0f;
        for (float v : rtts) {
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            tot += v;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%d/%d ok  min/avg/max %.1f/%.1f/%.1fms",
                 static_cast<int>(rtts.size()), sent, mn, tot / rtts.size(), mx);
        sum = buf;
    } else {
        sum = "0/" + std::to_string(sent) + " ok (100% loss)";
    }
    lines.push_back(sum);
    colors.push_back(rtts.empty() ? cfg::COL_RED : cfg::COL_ORANGE);

    ui::waitKeyRelease();
    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        std::string hdr = "Ping " + ui::fitCols(host, 18);
        ui::drawHeader(hdr.c_str());
        for (size_t i = 0; i < lines.size(); ++i) {
            cv.setTextColor(colors[i], cfg::COL_BG);
            cv.drawString(ui::fitCols(lines[i], 38).c_str(), 8, 25 + static_cast<int>(i) * 15);
        }
        ui::drawHints("Done", "Q / Enter back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || k.enter || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        delay(25);
    }
}

bool showConnectionDetail(const std::string& ssid) {
    ui::waitKeyRelease();
    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        std::string hdr = "Wi-Fi: " + ssid;
        ui::drawHeader(hdr.c_str(), g_conn_static ? "STATIC" : "DHCP",
                       g_conn_static ? cfg::COL_YELLOW : cfg::COL_GREEN);

        cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        cv.drawString(("IP:   " + std::string(WiFi.localIP().toString().c_str())).c_str(), 8, 26);
        cv.drawString(("Mask: " + std::string(WiFi.subnetMask().toString().c_str())).c_str(), 8, 41);
        cv.drawString(("GW:   " + std::string(WiFi.gatewayIP().toString().c_str())).c_str(), 8, 56);
        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString(("DNS:  " + std::string(WiFi.dnsIP().toString().c_str())).c_str(), 8, 71);
        std::string rssi_s = "RSSI: " + std::to_string(WiFi.RSSI()) + " dBm";
        cv.drawString(rssi_s.c_str(), 8, 86);

        ui::drawHints("P ping  D disc", "Q / Enter list");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || k.enter || lc == 'q') {
            ui::waitKeyRelease();
            return true;
        }
        if (lc == 'd') {
            WiFi.disconnect(false);
            g_conn_hidden = false;
            g_conn_static = false;
            ui::waitKeyRelease();
            return false;
        }
        if (lc == 'p') {
            runPingScreen();
            ui::waitKeyRelease();
        }
        delay(25);
    }
}

std::string suggestDefaultGateway(const std::string& ip) {
    size_t last_dot = ip.rfind('.');
    if (last_dot == std::string::npos || last_dot == 0) return "";
    return ip.substr(0, last_dot + 1) + "1";
}

bool chooseIpMode(const std::string& display_ssid, IpPlan& plan) {
    int sel = plan.use_dhcp ? 0 : 1;
    const char* opts[2] = {
        "1. DHCP (Auto IP)",
        "2. Static IP (Manual)",
    };
    ui::waitKeyRelease();
    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        std::string hdr = "IP: " + ui::fitCols(display_ssid, 22);
        ui::drawHeader(hdr.c_str());

        for (int i = 0; i < 2; ++i) {
            int y = 28 + i * 20;
            if (i == sel) {
                cv.fillRect(6, y - 2, 228, 17, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(opts[i], 12, y + 2);
        }

        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        if (!plan.ip.empty()) {
            std::string saved_s = "Saved: " + plan.ip + " / " + (plan.mask.empty() ? "255.255.255.0" : plan.mask);
            cv.drawString(ui::fitCols(saved_s, 37).c_str(), 10, 76);
        } else {
            cv.drawString("Use Static IP if AP has no DHCP", 10, 76);
        }
        ui::drawHints("Enter / 1-2 choose", "Q cancel");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return false;
        }
        if (k.up || k.down) sel = 1 - sel;
        else if (k.ch == '1') {
            plan.use_dhcp = true;
            ui::waitKeyRelease();
            return true;
        } else if (k.ch == '2') {
            plan.use_dhcp = false;
            ui::waitKeyRelease();
            return true;
        } else if (k.enter) {
            plan.use_dhcp = (sel == 0);
            ui::waitKeyRelease();
            return true;
        }
        delay(25);
    }
}

bool editStaticIpForm(IpPlan& plan) {
    if (plan.mask.empty()) plan.mask = "255.255.255.0";
    int cursor = 0;
    constexpr int kFields = 5;
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Static IP Config");

        const char* labels[kFields] = {"IP", "Mask", "Gateway", "DNS", "[ Connect ]"};
        std::string vals[kFields] = {
            plan.ip.empty() ? "(required)" : plan.ip,
            plan.mask,
            plan.gw.empty() ? "(optional)" : plan.gw,
            plan.dns.empty() ? "(optional)" : plan.dns,
            ""
        };

        for (int i = 0; i < kFields; ++i) {
            int y = 24 + i * 18;
            bool sel = (i == cursor);
            if (sel) {
                cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(i == 4 ? cfg::COL_GREEN : cfg::COL_CREAM, cfg::COL_BG);
            }
            std::string row = labels[i];
            if (!vals[i].empty()) row += ": " + vals[i];
            cv.drawString(ui::fitCols(row, 37).c_str(), 8, y + 2);
        }
        ui::drawHints("Enter edit/conn", "S conn  Q cancel");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return false;
        }
        if (k.up) cursor = (cursor + kFields - 1) % kFields;
        else if (k.down) cursor = (cursor + 1) % kFields;
        else if (lc == 's' || (k.enter && cursor == 4)) {
            if (plan.mask.empty()) plan.mask = "255.255.255.0";
            if (!isValidIpv4Literal(plan.ip)) {
                ui::showToast("Invalid IP", "Enter a valid IPv4 address", "e.g. 192.168.1.50", cfg::COL_RED);
                continue;
            }
            if (!isValidIpv4Literal(plan.mask)) {
                ui::showToast("Invalid Mask", "Enter a valid subnet mask", "e.g. 255.255.255.0", cfg::COL_RED);
                continue;
            }
            if (!plan.gw.empty() && !isValidIpv4Literal(plan.gw)) {
                ui::showToast("Invalid Gateway", "Leave empty or valid IPv4", "", cfg::COL_RED);
                continue;
            }
            if (!plan.dns.empty() && !isValidIpv4Literal(plan.dns)) {
                ui::showToast("Invalid DNS", "Leave empty or valid IPv4", "", cfg::COL_RED);
                continue;
            }
            ui::waitKeyRelease();
            return true;
        } else if (k.enter) {
            if (cursor == 0) {
                std::string prev = plan.ip;
                if (ui::promptText("Static IP", "e.g. 192.168.1.50", plan.ip, false, 15)) {
                    if (plan.gw.empty() && isValidIpv4Literal(plan.ip) && plan.ip != prev) {
                        plan.gw = suggestDefaultGateway(plan.ip);
                    }
                }
            } else if (cursor == 1) {
                ui::promptText("Subnet Mask", "e.g. 255.255.255.0", plan.mask, false, 15);
            } else if (cursor == 2) {
                ui::promptText("Gateway (optional)", "Empty for 0.0.0.0", plan.gw, false, 15);
            } else if (cursor == 3) {
                ui::promptText("DNS (optional)", "Empty for 0.0.0.0", plan.dns, false, 15);
            }
        }
        delay(25);
    }
}

bool connectToWifiFlow(const std::string& target_ssid, const std::string& pw, bool is_open,
                       bool is_hidden, const std::string& hint_bssid, bool used_saved_pw) {
    ensureWifiEvents();

    auto existing_list = storage::loadWifi();
    IpPlan plan;
    bool entry_hidden = is_hidden;
    for (const auto& w : existing_list) {
        if (w.ssid == target_ssid) {
            plan.use_dhcp = w.use_dhcp;
            plan.ip = w.static_ip;
            if (!w.subnet.empty()) plan.mask = w.subnet;
            plan.gw = w.gateway;
            if (!w.dns.empty() || !w.use_dhcp) plan.dns = w.dns;
            entry_hidden = entry_hidden || w.hidden;
            break;
        }
    }

    std::string disp_title = entry_hidden ? ("*" + target_ssid) : target_ssid;
    if (!chooseIpMode(disp_title, plan)) {
        return false;
    }
    if (!plan.use_dhcp && !editStaticIpForm(plan)) {
        return false;
    }

    bool cancelled = false;
    bool ever_l2 = false;
    bool no_dhcp_aborted = false;

    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect(false);
            delay(150);
        }
        WiFi.persistent(false);
        WiFi.setAutoReconnect(false);
        WiFi.mode(WIFI_STA);

        g_sta_l2_connected = false;
        g_sta_got_ip = false;

        if (plan.use_dhcp) {
            WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0),
                        IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
        } else {
            IPAddress ip, mask, gw(0, 0, 0, 0), dns(0, 0, 0, 0);
            ip.fromString(plan.ip.c_str());
            mask.fromString((plan.mask.empty() ? "255.255.255.0" : plan.mask).c_str());
            if (!plan.gw.empty()) gw.fromString(plan.gw.c_str());
            if (!plan.dns.empty()) dns.fromString(plan.dns.c_str());
            WiFi.config(ip, gw, mask, dns);
        }

        if (is_open || pw.empty()) {
            WiFi.begin(target_ssid.c_str());
        } else {
            WiFi.begin(target_ssid.c_str(), pw.c_str());
        }

        uint32_t t0 = millis();
        uint32_t l2_since = 0;
        bool switch_to_static = false;
        cancelled = false;
        ever_l2 = false;
        no_dhcp_aborted = false;

        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
            if (g_sta_l2_connected && !ever_l2) {
                ever_l2 = true;
                l2_since = millis();
            }

            auto& cv = ui::canvas();
            cv.fillScreen(cfg::COL_BG);
            ui::drawHeader("Wi-Fi Connecting", plan.use_dhcp ? "DHCP" : "STATIC",
                           plan.use_dhcp ? cfg::COL_GRAY : cfg::COL_YELLOW);
            cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            cv.drawString(ui::fitCols(disp_title, 36).c_str(), 10, 42);
            cv.setTextColor(ever_l2 ? cfg::COL_GREEN : cfg::COL_GRAY, cfg::COL_BG);
            const char* st_msg = !ever_l2 ? "Associating AP..."
                                          : (plan.use_dhcp ? "AP linked! Waiting DHCP..."
                                                           : "Applying Static IP...");
            cv.drawString(st_msg, 10, 62);
            ui::drawHints((plan.use_dhcp && ever_l2) ? "S static IP" : "Connecting...", "Q cancel");
            cv.pushSprite(0, 0);

            KeyInput ck = ui::pollInput();
            char clc = ck.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(ck.ch))) : 0;
            if (ck.esc || ck.btn_g0 || clc == 'q') {
                cancelled = true;
                break;
            }
            if (plan.use_dhcp && ever_l2 && clc == 's') {
                switch_to_static = true;
                break;
            }
            if (plan.use_dhcp && ever_l2 && (millis() - l2_since >= 7000)) {
                WiFi.disconnect(false);
                if (ui::promptConfirm("No DHCP Server", "AP linked, no DHCP reply", "Configure Static IP now?")) {
                    switch_to_static = true;
                } else {
                    no_dhcp_aborted = true;
                }
                break;
            }
            delay(80);
        }

        if (switch_to_static) {
            WiFi.disconnect(false);
            plan.use_dhcp = false;
            if (editStaticIpForm(plan)) {
                continue;
            }
            cancelled = true;
        }
        break;
    }

    if (WiFi.status() == WL_CONNECTED) {
        std::string connected_bssid = formatMacLower(WiFi.BSSID());
        if (connected_bssid.empty()) connected_bssid = hint_bssid;

        auto list = storage::loadWifi();
        bool updated = false;
        for (auto& item : list) {
            if (item.ssid == target_ssid) {
                item.password = pw;
                item.hidden = item.hidden || entry_hidden;
                if (item.hidden && !connected_bssid.empty()) {
                    appendBssid(item.bssid, connected_bssid);
                }
                item.use_dhcp = plan.use_dhcp;
                if (!plan.use_dhcp) {
                    item.static_ip = plan.ip;
                    item.subnet = plan.mask;
                    item.gateway = plan.gw;
                    item.dns = plan.dns;
                }
                updated = true;
                break;
            }
        }
        if (!updated) {
            WifiEntry nw;
            nw.ssid = target_ssid;
            nw.password = pw;
            nw.hidden = entry_hidden;
            if (entry_hidden && !connected_bssid.empty()) {
                appendBssid(nw.bssid, connected_bssid);
            }
            nw.use_dhcp = plan.use_dhcp;
            if (!plan.use_dhcp) {
                nw.static_ip = plan.ip;
                nw.subnet = plan.mask;
                nw.gateway = plan.gw;
                nw.dns = plan.dns;
            }
            list.push_back(nw);
        }
        storage::saveWifi(list);

        g_conn_hidden = entry_hidden;
        g_conn_static = !plan.use_dhcp;
        if (!time_sync::isTimeSynced()) {
            time_sync::startAsyncSync();
        }
        showConnectionDetail(entry_hidden ? ("*" + target_ssid) : target_ssid);
        ui::waitKeyRelease();
        return true;
    }

    WiFi.disconnect(false);
    g_conn_hidden = false;
    g_conn_static = false;
    if (used_saved_pw && !cancelled && !ever_l2 && !no_dhcp_aborted) {
        auto list = storage::loadWifi();
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const WifiEntry& w) { return w.ssid == target_ssid; }),
                   list.end());
        storage::saveWifi(list);
    }
    const char* reason = cancelled ? "Cancelled"
                                   : (no_dhcp_aborted ? "No DHCP server"
                                                      : (ever_l2 ? "DHCP / IP timeout"
                                                                 : (used_saved_pw ? "Cleared saved pw"
                                                                                  : "Wrong password or timeout")));
    ui::showToast("Wi-Fi Failed", disp_title, reason, cfg::COL_RED);
    ui::waitKeyRelease();
    return false;
}

void runSavedWifiScreen() {
    auto saved = storage::loadWifi();
    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 5;
    ui::waitKeyRelease();

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        bool connected = (WiFi.status() == WL_CONNECTED);
        ui::drawHeader("Saved Wi-Fi");

        if (saved.empty()) {
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString("No saved Wi-Fi networks", 48, 56);
        } else {
            for (int i = 0; i < kMaxVis && (scroll + i) < static_cast<int>(saved.size()); ++i) {
                int idx = scroll + i;
                int y = 26 + i * 18;
                bool sel = (idx == cursor);
                if (sel) {
                    cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                    cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
                } else {
                    cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
                }
                std::string label = saved[idx].hidden ? ("*" + saved[idx].ssid) : saved[idx].ssid;
                cv.drawString(ui::fitCols(label, 28).c_str(), 8, y + 2);

                if (!sel) cv.setTextColor(saved[idx].use_dhcp ? cfg::COL_CYAN : cfg::COL_YELLOW, cfg::COL_BG);
                cv.setTextDatum(top_right);
                cv.drawString(saved[idx].use_dhcp ? "[D]" : "[S]", 232, y + 2);
                cv.setTextDatum(top_left);
            }
        }
        ui::drawHints("Enter connect", "D del  Q back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        if (k.up && !saved.empty()) {
            cursor = (cursor - 1 + static_cast<int>(saved.size())) % static_cast<int>(saved.size());
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, static_cast<int>(saved.size()) - kMaxVis);
        } else if (k.down && !saved.empty()) {
            cursor = (cursor + 1) % static_cast<int>(saved.size());
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (lc == 'd' && !saved.empty()) {
            std::string label = saved[cursor].hidden ? ("*" + saved[cursor].ssid) : saved[cursor].ssid;
            if (ui::promptConfirm("Delete Wi-Fi?", label, "Remove from /wifi.csv?")) {
                saved.erase(saved.begin() + cursor);
                storage::saveWifi(saved);
                if (cursor >= static_cast<int>(saved.size()) && cursor > 0) --cursor;
                if (cursor < scroll) scroll = cursor;
            }
            ui::waitKeyRelease();
        } else if (k.enter && !saved.empty()) {
            const auto entry = saved[cursor];
            connectToWifiFlow(entry.ssid, entry.password, entry.password.empty(),
                              entry.hidden, "", !entry.password.empty());
            saved = storage::loadWifi();
            if (cursor >= static_cast<int>(saved.size()) && cursor > 0) --cursor;
            if (cursor < scroll) scroll = cursor;
            ui::waitKeyRelease();
        }
        delay(25);
    }
}

}  // namespace

void runWifiScannerApp() {
    ui::waitKeyRelease();
    auto aps = scanNetworksUi();
    ui::waitKeyRelease();
    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 5;

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        bool connected = (WiFi.status() == WL_CONNECTED);
        ui::drawHeader("Wi-Fi");

        if (aps.empty()) {
            cv.setTextColor(cfg::COL_YELLOW, cfg::COL_BG);
            cv.drawString("No networks found", 60, 56);
        } else {
            for (int i = 0; i < kMaxVis && (scroll + i) < static_cast<int>(aps.size()); ++i) {
                int idx = scroll + i;
                int y = 26 + i * 18;
                bool sel = (idx == cursor);
                if (sel) {
                    cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                    cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
                } else {
                    cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
                }
                cv.drawString(ui::fitCols(aps[idx].display, 22).c_str(), 8, y + 2);

                if (!sel) cv.setTextColor(signalColor(aps[idx].rssi), cfg::COL_BG);
                cv.setTextDatum(top_right);
                cv.drawString(std::to_string(aps[idx].rssi).c_str(), 192, y + 2);

                if (!sel) cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
                cv.drawString(aps[idx].auth.c_str(), 232, y + 2);
                cv.setTextDatum(top_left);
            }
        }
        ui::drawHints("Enter conn  S saved", connected ? "I info  R scan" : "R scan  Q back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        if (lc == 'i' && connected) {
            std::string cur_ssid = WiFi.SSID().c_str();
            showConnectionDetail(g_conn_hidden ? ("*" + cur_ssid) : cur_ssid);
            ui::waitKeyRelease();
        } else if (lc == 's') {
            runSavedWifiScreen();
            ui::waitKeyRelease();
        } else if (lc == 'r') {
            aps = scanNetworksUi();
            ui::waitKeyRelease();
            cursor = scroll = 0;
        } else if (k.up && !aps.empty()) {
            cursor = (cursor - 1 + static_cast<int>(aps.size())) % static_cast<int>(aps.size());
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, static_cast<int>(aps.size()) - kMaxVis);
        } else if (k.down && !aps.empty()) {
            cursor = (cursor + 1) % static_cast<int>(aps.size());
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (k.enter && !aps.empty()) {
            const auto ap = aps[cursor];
            bool already_here = false;
            if (connected) {
                if (!ap.hidden || ap.known_hidden) {
                    already_here = (std::string(WiFi.SSID().c_str()) == ap.ssid);
                } else if (!ap.bssid.empty()) {
                    already_here = equalsIgnoreCase(WiFi.BSSIDstr().c_str(), ap.bssid);
                }
            }
            if (already_here) {
                std::string cur_ssid = WiFi.SSID().c_str();
                showConnectionDetail((g_conn_hidden || ap.hidden) ? ("*" + cur_ssid) : cur_ssid);
                ui::waitKeyRelease();
                continue;
            }

            std::string target_ssid = ap.ssid;
            if (ap.hidden && !ap.known_hidden) {
                if (!ui::promptText("Hidden Wi-Fi SSID", ap.display, target_ssid, false, 32) || target_ssid.empty()) {
                    ui::waitKeyRelease();
                    continue;
                }
            }

            std::string pw;
            bool used_saved = false;
            if (!ap.open) {
                auto saved = storage::loadWifi();
                for (const auto& w : saved) {
                    if (w.ssid == target_ssid && !w.password.empty()) {
                        pw = w.password;
                        used_saved = true;
                        break;
                    }
                }
                if (pw.empty()) {
                    std::string sub = ap.hidden ? ("*" + target_ssid) : target_ssid;
                    if (!ui::promptText("Wi-Fi Password", sub, pw, true, 63) || pw.empty()) {
                        ui::waitKeyRelease();
                        continue;
                    }
                }
            }

            if (connectToWifiFlow(target_ssid, pw, ap.open, ap.hidden, ap.bssid, used_saved) && ap.hidden) {
                for (auto& row : aps) {
                    if (row.hidden && !ap.bssid.empty() && equalsIgnoreCase(row.bssid, ap.bssid)) {
                        row.ssid = target_ssid;
                        row.known_hidden = true;
                        row.display = "*" + target_ssid;
                    }
                }
            }
        }
        delay(25);
    }
}
