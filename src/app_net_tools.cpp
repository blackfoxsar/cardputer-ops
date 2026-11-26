#include "app_net_tools.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <sys/select.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "dns_wire.h"
#include "ui_common.h"

namespace {

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

bool resolveHost(const std::string& host, IPAddress& out_ip) {
    if (isValidIpv4Literal(host)) {
        return out_ip.fromString(host.c_str());
    }
    return WiFi.hostByName(host.c_str(), out_ip) == 1;
}

void parseHostPort(const std::string& raw, std::string& host, uint16_t& port, bool& has_port) {
    has_port = false;
    port = 0;
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
                has_port = true;
                return;
            }
        }
    }
    host = raw;
}

uint16_t readBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void buildIcmpEchoPacket(uint8_t* pkt, size_t len, uint16_t id, uint16_t seq) {
    std::memset(pkt, 0, len);
    pkt[0] = 8;  // ICMP Echo Request
    pkt[1] = 0;
    pkt[4] = static_cast<uint8_t>((id >> 8) & 0xFF);
    pkt[5] = static_cast<uint8_t>(id & 0xFF);
    pkt[6] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    pkt[7] = static_cast<uint8_t>(seq & 0xFF);
    for (size_t i = 8; i < len; ++i) {
        pkt[i] = static_cast<uint8_t>('A' + (i % 26));
    }
    uint16_t csum = dns_wire::checksum16(pkt, len);
    pkt[2] = static_cast<uint8_t>((csum >> 8) & 0xFF);
    pkt[3] = static_cast<uint8_t>(csum & 0xFF);
}

int openRawIcmpSocket() {
    int sock = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) return -1;
    int flags = lwip_fcntl(sock, F_GETFL, 0);
    lwip_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    return sock;
}

struct IcmpOutcome {
    enum class Kind { TIMEOUT, ECHO_REPLY, TIME_EXCEEDED, UNREACHABLE, ABORTED };
    Kind kind = Kind::TIMEOUT;
    uint8_t ttl = 0;
    float rtt_ms = 0.0f;
    std::string responder_ip;
};

IcmpOutcome sendAndWaitIcmp(int sock, const IPAddress& dst_ip, int send_ttl,
                            uint16_t id, uint16_t seq, uint32_t timeout_ms, bool* out_aborted) {
    IcmpOutcome res;
    if (out_aborted) *out_aborted = false;

    int ttl_opt = send_ttl;
    lwip_setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl_opt, sizeof(ttl_opt));

    // Drain any stale queued packets before sending this probe.
    uint8_t drain[128];
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    while (lwip_recvfrom(sock, drain, sizeof(drain), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len) > 0) {
        from_len = sizeof(from);
    }

    uint8_t tx[40];
    buildIcmpEchoPacket(tx, sizeof(tx), id, seq);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = static_cast<uint32_t>(dst_ip);

    uint64_t t0 = esp_timer_get_time();
    if (lwip_sendto(sock, tx, sizeof(tx), 0,
                    reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) < 0) {
        return res;
    }

    uint8_t rx[256];
    uint32_t start_ms = millis();
    while (millis() - start_ms < timeout_ms) {
        KeyInput k = ui::pollInput();
        if (k.esc || k.btn_g0) {
            res.kind = IcmpOutcome::Kind::ABORTED;
            if (out_aborted) *out_aborted = true;
            return res;
        }

        from_len = sizeof(from);
        ssize_t n = lwip_recvfrom(sock, rx, sizeof(rx), 0,
                                  reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n >= 28) {
            uint8_t ihl = static_cast<uint8_t>((rx[0] & 0x0F) * 4);
            if (ihl >= 20 && n >= static_cast<ssize_t>(ihl + 8)) {
                const uint8_t* icmp = rx + ihl;
                uint8_t type = icmp[0];
                float dt = static_cast<float>(esp_timer_get_time() - t0) / 1000.0f;

                char ip_buf[20];
                std::snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
                              rx[12], rx[13], rx[14], rx[15]);

                if (type == 0) {  // Echo Reply
                    if (readBe16(icmp + 4) == id && readBe16(icmp + 6) == seq) {
                        res.kind = IcmpOutcome::Kind::ECHO_REPLY;
                        res.ttl = rx[8];
                        res.rtt_ms = dt;
                        res.responder_ip = ip_buf;
                        return res;
                    }
                } else if (type == 11 || type == 3) {  // Time Exceeded / Dest Unreachable
                    size_t icmp_len = static_cast<size_t>(n) - ihl;
                    if (icmp_len >= 28) {
                        const uint8_t* qip = icmp + 8;
                        uint8_t qihl = static_cast<uint8_t>((qip[0] & 0x0F) * 4);
                        if (qihl >= 20 && icmp_len >= static_cast<size_t>(8 + qihl + 8)) {
                            const uint8_t* qicmp = qip + qihl;
                            if (qicmp[0] == 8 &&
                                readBe16(qicmp + 4) == id &&
                                readBe16(qicmp + 6) == seq) {
                                res.kind = (type == 11) ? IcmpOutcome::Kind::TIME_EXCEEDED
                                                        : IcmpOutcome::Kind::UNREACHABLE;
                                res.ttl = rx[8];
                                res.rtt_ms = dt;
                                res.responder_ip = ip_buf;
                                return res;
                            }
                        }
                    }
                }
            }
        }
        delay(15);
    }
    return res;
}

struct TcpProbeResult {
    bool ok = false;
    bool rst = false;
    bool aborted = false;
    float rtt_ms = 0.0f;
};

TcpProbeResult tcpPingOnce(const IPAddress& ip, uint16_t port, uint32_t timeout_ms) {
    TcpProbeResult res;
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
        uint32_t t_start = millis();
        while (millis() - t_start < timeout_ms) {
            KeyInput k = ui::pollInput();
            if (k.esc || k.btn_g0) {
                res.aborted = true;
                break;
            }
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            timeval tv{0, 20000};
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
                break;
            }
        }
    }
    lwip_close(sock);
    return res;
}

// ── Ping Tool ─────────────────────────────────────────────────────────

void runPingApp() {
    if (WiFi.status() != WL_CONNECTED) {
        ui::showToast("Wi-Fi Offline", "Connect Wi-Fi in Settings first", "", cfg::COL_RED);
        return;
    }

    static std::string last_target = "8.8.8.8";
    std::string target = last_target;
    if (!ui::promptText("Ping", "Host/IP or host:port (TCP)", target, false, 63) || target.empty()) {
        return;
    }
    last_target = target;

    std::string host;
    uint16_t tcp_port = 0;
    bool use_tcp = false;
    parseHostPort(target, host, tcp_port, use_tcp);

    IPAddress ip;
    if (!resolveHost(host, ip)) {
        ui::showToast("Ping Failed", "DNS resolve failed:", host, cfg::COL_RED);
        return;
    }

    int raw_sock = -1;
    if (!use_tcp) {
        raw_sock = openRawIcmpSocket();
        if (raw_sock < 0) {
            ui::showToast("ICMP Error", "Raw socket unavailable", "Try host:port for TCP ping", cfg::COL_RED);
            return;
        }
    }

    std::vector<std::string> log_lines;
    std::vector<uint16_t> log_cols;
    int tx_cnt = 0;
    int rx_cnt = 0;
    float rtt_min = 0.0f, rtt_max = 0.0f, rtt_sum = 0.0f;
    bool paused = false;
    uint16_t seq = 1;
    constexpr int kMaxLines = 5;

    auto drawUi = [&]() {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        std::string hdr = (use_tcp ? "TCP " : "Ping ") + ui::fitCols(host, 12);
        ui::drawHeader(hdr.c_str());

        std::string sub = std::string(ip.toString().c_str()) +
                          (use_tcp ? (":" + std::to_string(tcp_port)) : " (ICMP)") +
                          (paused ? " [PAUSED]" : "");
        cv.setTextColor(paused ? cfg::COL_YELLOW : cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString(ui::fitCols(sub, 37).c_str(), 8, 24);

        size_t start = (log_lines.size() > kMaxLines) ? (log_lines.size() - kMaxLines) : 0;
        for (size_t i = start; i < log_lines.size(); ++i) {
            int row = static_cast<int>(i - start);
            cv.setTextColor(log_cols[i], cfg::COL_BG);
            cv.drawString(ui::fitCols(log_lines[i], 38).c_str(), 8, 38 + row * 12);
        }

        char stats[48];
        if (rx_cnt > 0) {
            int loss = ((tx_cnt - rx_cnt) * 100) / std::max(1, tx_cnt);
            std::snprintf(stats, sizeof(stats), "Tx:%d Rx:%d Loss:%d%% Avg:%.1fms",
                          tx_cnt, rx_cnt, loss, rtt_sum / rx_cnt);
            cv.setTextColor(cfg::COL_CYAN, cfg::COL_BG);
        } else {
            std::snprintf(stats, sizeof(stats), "Tx:%d Rx:0 (waiting...)", tx_cnt);
            cv.setTextColor(tx_cnt > 0 ? cfg::COL_RED : cfg::COL_GRAY, cfg::COL_BG);
        }
        cv.drawString(ui::fitCols(stats, 38).c_str(), 8, 102);

        ui::drawHints(paused ? "Space resume  R reset" : "Space pause  R reset", "ESC / G0 back");
        cv.pushSprite(0, 0);
    };

    ui::waitKeyRelease();
    uint32_t next_probe_ms = 0;

    while (true) {
        drawUi();

        KeyInput k = ui::pollInput();
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0) {
            break;
        }
        if (k.ch == ' ') {
            paused = !paused;
            next_probe_ms = millis();
        } else if (lc == 'r') {
            log_lines.clear();
            log_cols.clear();
            tx_cnt = rx_cnt = 0;
            rtt_min = rtt_max = rtt_sum = 0.0f;
            seq = 1;
            next_probe_ms = 0;
        }

        if (!paused && millis() >= next_probe_ms) {
            ++tx_cnt;
            bool aborted = false;
            if (use_tcp) {
                TcpProbeResult pr = tcpPingOnce(ip, tcp_port, 1200);
                if (pr.aborted) {
                    aborted = true;
                } else if (pr.ok) {
                    ++rx_cnt;
                    rtt_min = (rx_cnt == 1) ? pr.rtt_ms : std::min(rtt_min, pr.rtt_ms);
                    rtt_max = (rx_cnt == 1) ? pr.rtt_ms : std::max(rtt_max, pr.rtt_ms);
                    rtt_sum += pr.rtt_ms;
                    char line[48];
                    std::snprintf(line, sizeof(line), "seq=%u tcp/%.0fms%s",
                                  seq, pr.rtt_ms, pr.rst ? " (rst)" : "");
                    log_lines.push_back(line);
                    log_cols.push_back(cfg::COL_GREEN);
                } else {
                    log_lines.push_back("seq=" + std::to_string(seq) + " timeout");
                    log_cols.push_back(cfg::COL_RED);
                }
            } else {
                IcmpOutcome io = sendAndWaitIcmp(raw_sock, ip, 64, 0x4350, seq, 1000, &aborted);
                if (!aborted) {
                    if (io.kind == IcmpOutcome::Kind::ECHO_REPLY) {
                        ++rx_cnt;
                        rtt_min = (rx_cnt == 1) ? io.rtt_ms : std::min(rtt_min, io.rtt_ms);
                        rtt_max = (rx_cnt == 1) ? io.rtt_ms : std::max(rtt_max, io.rtt_ms);
                        rtt_sum += io.rtt_ms;
                        char line[48];
                        std::snprintf(line, sizeof(line), "seq=%u ttl=%u %.1f ms",
                                      seq, static_cast<unsigned>(io.ttl), io.rtt_ms);
                        log_lines.push_back(line);
                        log_cols.push_back(cfg::COL_GREEN);
                    } else if (io.kind == IcmpOutcome::Kind::UNREACHABLE) {
                        log_lines.push_back("seq=" + std::to_string(seq) + " unreach " + io.responder_ip);
                        log_cols.push_back(cfg::COL_YELLOW);
                    } else {
                        log_lines.push_back("seq=" + std::to_string(seq) + " timeout");
                        log_cols.push_back(cfg::COL_RED);
                    }
                }
            }
            if (aborted) break;
            ++seq;
            next_probe_ms = millis() + 800;
        }
        delay(20);
    }

    if (raw_sock >= 0) lwip_close(raw_sock);
    ui::waitKeyRelease();
}

// ── Dig (DNS Lookup) Tool ─────────────────────────────────────────────

void executeDigQuery(const std::string& domain, uint16_t qtype, const std::string& server_cfg) {
    IPAddress dns_ip;
    if (server_cfg.empty() || server_cfg == "Auto") {
        dns_ip = WiFi.dnsIP();
        if (static_cast<uint32_t>(dns_ip) == 0) {
            dns_ip.fromString("8.8.8.8");
        }
    } else if (!resolveHost(server_cfg, dns_ip)) {
        ui::showToast("Dig Error", "Invalid DNS server:", server_cfg, cfg::COL_RED);
        return;
    }

    uint16_t txid = static_cast<uint16_t>(esp_random() & 0xFFFFU);
    std::vector<uint8_t> req;
    if (!dns_wire::buildQuery(txid, domain, qtype, req)) {
        ui::showToast("Dig Error", "Invalid domain name:", domain, cfg::COL_RED);
        return;
    }

    auto& cv = ui::canvas();
    cv.fillScreen(cfg::COL_BG);
    ui::drawHeader("Dig Query");
    cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
    cv.drawString(ui::fitCols(domain + " (" + dns_wire::typeName(qtype) + ")", 36).c_str(), 10, 44);
    cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
    cv.drawString(("Server: " + std::string(dns_ip.toString().c_str())).c_str(), 10, 64);
    ui::drawHints("Querying UDP/53...", "ESC cancel");
    cv.pushSprite(0, 0);

    WiFiUDP udp;
    if (!udp.begin(0)) {
        ui::showToast("Dig Error", "Failed to open UDP socket", "", cfg::COL_RED);
        return;
    }

    uint64_t t0 = esp_timer_get_time();
    udp.beginPacket(dns_ip, 53);
    udp.write(req.data(), req.size());
    udp.endPacket();

    uint8_t rx[512];
    int rx_len = 0;
    uint32_t start_ms = millis();
    bool cancelled = false;
    while (millis() - start_ms < 3000) {
        int pkt_sz = udp.parsePacket();
        if (pkt_sz > 0) {
            int n = udp.read(rx, sizeof(rx));
            if (n >= 12) {
                rx_len = n;
                break;
            }
        }
        KeyInput k = ui::pollInput();
        if (k.esc || k.btn_g0) {
            cancelled = true;
            break;
        }
        delay(20);
    }
    float elapsed_ms = static_cast<float>(esp_timer_get_time() - t0) / 1000.0f;
    udp.stop();

    if (cancelled) {
        ui::waitKeyRelease();
        return;
    }
    if (rx_len == 0) {
        ui::showToast("Dig Timeout", "No response from " + std::string(dns_ip.toString().c_str()),
                      "UDP/53 timed out (3.0s)", cfg::COL_RED);
        return;
    }

    dns_wire::DnsResponse resp = dns_wire::parseResponse(rx, static_cast<size_t>(rx_len), txid);
    if (!resp.ok) {
        ui::showToast("Dig Parse Error", resp.error, "", cfg::COL_RED);
        return;
    }

    std::vector<std::string> lines;
    std::vector<uint16_t> cols;
    char hdr_buf[64];
    std::snprintf(hdr_buf, sizeof(hdr_buf), "%s  @%s  %.0fms",
                  dns_wire::rcodeName(resp.rcode), dns_ip.toString().c_str(), elapsed_ms);
    lines.push_back(hdr_buf);
    cols.push_back(resp.rcode == 0 ? cfg::COL_GREEN : cfg::COL_RED);

    if (resp.answers.empty()) {
        lines.push_back("(0 answers for " + std::string(dns_wire::typeName(qtype)) + ")");
        cols.push_back(cfg::COL_YELLOW);
    } else {
        for (const auto& ans : resp.answers) {
            std::string row = std::string(dns_wire::typeName(ans.type)) + " " +
                              std::to_string(ans.ttl) + "s  " + ans.rdata;
            // Wrap long records across lines so TXT/SOA/AAAA remain readable.
            for (size_t pos = 0; pos < row.size(); pos += 37) {
                lines.push_back(row.substr(pos, 37));
                cols.push_back(cfg::COL_CREAM);
            }
        }
    }

    constexpr size_t kMaxVis = 6;
    size_t scroll = 0;
    ui::waitKeyRelease();
    while (true) {
        cv.fillScreen(cfg::COL_BG);
        std::string title = "Dig " + ui::fitCols(domain, 12);
        ui::drawHeader(title.c_str());

        for (size_t i = 0; i < kMaxVis && (scroll + i) < lines.size(); ++i) {
            cv.setTextColor(cols[scroll + i], cfg::COL_BG);
            cv.drawString(lines[scroll + i].c_str(), 8, 25 + static_cast<int>(i) * 14);
        }

        bool scrollable = lines.size() > kMaxVis;
        ui::drawHints(scrollable ? ";. scroll" : "Done", "ESC / G0 back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        if (k.esc || k.btn_g0) {
            ui::waitKeyRelease();
            return;
        }
        if (k.up && scroll > 0) --scroll;
        else if (k.down && scrollable && (scroll + kMaxVis) < lines.size()) ++scroll;
        delay(25);
    }
}

void runDigApp() {
    if (WiFi.status() != WL_CONNECTED) {
        ui::showToast("Wi-Fi Offline", "Connect Wi-Fi in Settings first", "", cfg::COL_RED);
        return;
    }

    static std::string s_domain = "example.com";
    static int s_type_idx = 0;
    static std::string s_server = "Auto";

    const uint16_t kTypes[] = {
        dns_wire::kTypeA, dns_wire::kTypeAaaa, dns_wire::kTypeCname,
        dns_wire::kTypeMx, dns_wire::kTypeTxt, dns_wire::kTypeNs, dns_wire::kTypePtr
    };
    constexpr int kNumTypes = sizeof(kTypes) / sizeof(kTypes[0]);

    int cursor = 0;
    constexpr int kRows = 4;
    ui::waitKeyRelease();

    while (true) {
        std::string cur_dns = (s_server == "Auto")
                                  ? ("Auto (" + std::string(WiFi.dnsIP().toString().c_str()) + ")")
                                  : s_server;
        const std::string rows[kRows] = {
            "Domain: " + s_domain,
            std::string("Type:   ") + dns_wire::typeName(kTypes[s_type_idx]),
            "Server: " + cur_dns,
            "[ Run Dig Query ]"
        };

        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Dig (DNS)");
        for (int i = 0; i < kRows; ++i) {
            int y = 26 + i * 21;
            if (i == cursor) {
                cv.fillRect(6, y - 2, 228, 17, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(i == 3 ? cfg::COL_GREEN : cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(ui::fitCols(rows[i], 36).c_str(), 10, y + 2);
        }
        ui::drawHints(cursor == 1 ? ",/ cycle type  G run" : "Enter edit  G run", "ESC / G0 back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0) {
            ui::waitKeyRelease();
            return;
        }
        if (k.up) cursor = (cursor + kRows - 1) % kRows;
        else if (k.down) cursor = (cursor + 1) % kRows;
        else if (cursor == 1 && (k.left || k.right)) {
            s_type_idx = (s_type_idx + (k.right ? 1 : (kNumTypes - 1))) % kNumTypes;
        } else if (lc == 'g' || (k.enter && cursor == 3)) {
            executeDigQuery(s_domain, kTypes[s_type_idx], s_server);
            ui::waitKeyRelease();
        } else if (k.enter) {
            if (cursor == 0) {
                ui::promptText("DNS Domain", "e.g. github.com", s_domain, false, 63);
            } else if (cursor == 1) {
                s_type_idx = (s_type_idx + 1) % kNumTypes;
            } else if (cursor == 2) {
                std::string srv = (s_server == "Auto") ? "" : s_server;
                if (ui::promptText("DNS Server", "IP (blank = Auto Wi-Fi DNS)", srv, false, 40)) {
                    s_server = srv.empty() ? "Auto" : srv;
                }
            }
            ui::waitKeyRelease();
        }
        delay(25);
    }
}

// ── MTR (Trace + Live Loss/RTT) Tool ──────────────────────────────────

struct MtrHop {
    std::string ip;
    int sent = 0;
    int recv = 0;
    float last_ms = 0.0f;
    float sum_ms = 0.0f;
};

void runMtrApp() {
    if (WiFi.status() != WL_CONNECTED) {
        ui::showToast("Wi-Fi Offline", "Connect Wi-Fi in Settings first", "", cfg::COL_RED);
        return;
    }

    static std::string last_target = "1.1.1.1";
    std::string target = last_target;
    if (!ui::promptText("MTR Traceroute", "Enter target host or IP", target, false, 63) || target.empty()) {
        return;
    }
    last_target = target;

    IPAddress dst_ip;
    if (!resolveHost(target, dst_ip)) {
        ui::showToast("MTR Failed", "DNS resolve failed:", target, cfg::COL_RED);
        return;
    }

    int raw_sock = openRawIcmpSocket();
    if (raw_sock < 0) {
        ui::showToast("ICMP Error", "Raw socket unavailable", "", cfg::COL_RED);
        return;
    }

    constexpr int kMaxHops = 16;
    constexpr int kVisHops = 5;
    std::vector<MtrHop> hops(kMaxHops);
    int known_max_ttl = kMaxHops;
    int cur_ttl = 1;
    int round = 0;
    int scroll = 0;
    bool paused = false;
    bool reached = false;
    std::string dst_str = dst_ip.toString().c_str();

    auto drawUi = [&]() {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        std::string hdr = "MTR " + ui::fitCols(target, 14);
        ui::drawHeader(hdr.c_str());

        cv.setTextColor(paused ? cfg::COL_YELLOW : cfg::COL_GRAY, cfg::COL_BG);
        std::string col_hdr = paused ? "# Host (PAUSED)    Loss%  Avg"
                                     : "# Host             Loss%  Avg";
        cv.drawString(col_hdr.c_str(), 6, 24);

        int show_count = reached ? known_max_ttl : std::min(kMaxHops, std::max(cur_ttl, known_max_ttl));
        if (scroll > std::max(0, show_count - kVisHops)) {
            scroll = std::max(0, show_count - kVisHops);
        }

        for (int i = 0; i < kVisHops && (scroll + i) < show_count; ++i) {
            int idx = scroll + i;
            const auto& h = hops[idx];
            int y = 37 + i * 15;
            bool is_probe = (!paused && idx + 1 == cur_ttl);
            uint16_t row_col = h.ip.empty() ? cfg::COL_GRAY
                                            : ((h.ip == dst_str) ? cfg::COL_GREEN : cfg::COL_CREAM);
            if (is_probe) row_col = cfg::COL_ORANGE;

            char line[48];
            if (h.sent == 0) {
                std::snprintf(line, sizeof(line), "%2d ...             --     --", idx + 1);
            } else {
                int loss = ((h.sent - h.recv) * 100) / h.sent;
                std::string hop_ip = h.ip.empty() ? "*" : h.ip;
                if (h.recv > 0) {
                    std::snprintf(line, sizeof(line), "%2d %-15s %3d%% %5.1f",
                                  idx + 1, hop_ip.c_str(), loss, h.sum_ms / h.recv);
                } else {
                    std::snprintf(line, sizeof(line), "%2d %-15s %3d%%    --",
                                  idx + 1, hop_ip.c_str(), loss);
                }
            }
            cv.setTextColor(row_col, cfg::COL_BG);
            cv.drawString(ui::fitCols(line, 38).c_str(), 6, y);
        }

        ui::drawHints(paused ? "Space run  R rst ;." : "Space pause R rst ;.", "ESC / G0 back");
        cv.pushSprite(0, 0);
    };

    ui::waitKeyRelease();

    while (true) {
        drawUi();

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0) break;

        if (k.ch == ' ') {
            paused = !paused;
        } else if (lc == 'r') {
            hops.assign(kMaxHops, MtrHop{});
            known_max_ttl = kMaxHops;
            cur_ttl = 1;
            round = 0;
            scroll = 0;
            reached = false;
        } else if (k.up && scroll > 0) {
            --scroll;
        } else if (k.down) {
            int max_sc = std::max(0, (reached ? known_max_ttl : kMaxHops) - kVisHops);
            if (scroll < max_sc) ++scroll;
        }

        if (!paused) {
            int idx = cur_ttl - 1;
            uint16_t seq = static_cast<uint16_t>(((round & 0x7FF) << 5) | (cur_ttl & 0x1F));
            bool aborted = false;
            IcmpOutcome io = sendAndWaitIcmp(raw_sock, dst_ip, cur_ttl, 0x4D54, seq, 420, &aborted);
            if (aborted) break;

            hops[idx].sent++;
            if (io.kind == IcmpOutcome::Kind::ECHO_REPLY ||
                io.kind == IcmpOutcome::Kind::TIME_EXCEEDED ||
                io.kind == IcmpOutcome::Kind::UNREACHABLE) {
                hops[idx].ip = io.responder_ip;
                hops[idx].recv++;
                hops[idx].last_ms = io.rtt_ms;
                hops[idx].sum_ms += io.rtt_ms;
                if (io.kind == IcmpOutcome::Kind::ECHO_REPLY ||
                    io.kind == IcmpOutcome::Kind::UNREACHABLE) {
                    known_max_ttl = cur_ttl;
                    reached = true;
                }
            }

            if (cur_ttl >= known_max_ttl) {
                cur_ttl = 1;
                ++round;
            } else {
                ++cur_ttl;
            }
        }
        delay(20);
    }

    lwip_close(raw_sock);
    ui::waitKeyRelease();
}

}  // namespace

void runNetToolsMenu() {
    int cursor = 0;
    const char* items[3] = {
        "1. Ping (ICMP / TCP)",
        "2. Dig (DNS Lookup)",
        "3. MTR (Traceroute)",
    };

    ui::waitKeyRelease();
    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Net Tools");

        for (int i = 0; i < 3; ++i) {
            int y = 30 + i * 22;
            if (i == cursor) {
                cv.fillRect(6, y - 2, 228, 18, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(items[i], 12, y + 2);
        }
        ui::drawHints("Enter / 1-3 open", "ESC / G0 back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        if (k.esc || k.btn_g0) {
            ui::waitKeyRelease();
            return;
        }

        int open_idx = -1;
        if (k.up) cursor = (cursor + 2) % 3;
        else if (k.down) cursor = (cursor + 1) % 3;
        else if (k.enter) open_idx = cursor;
        else if (k.ch >= '1' && k.ch <= '3') {
            cursor = k.ch - '1';
            open_idx = cursor;
        }

        if (open_idx == 0) {
            runPingApp();
            ui::waitKeyRelease();
        } else if (open_idx == 1) {
            runDigApp();
            ui::waitKeyRelease();
        } else if (open_idx == 2) {
            runMtrApp();
            ui::waitKeyRelease();
        }
        delay(25);
    }
}
