#include "dns_wire.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace dns_wire {

namespace {

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t readU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

void writeU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

bool decodeName(const uint8_t* pkt, size_t len, size_t offset,
                std::string& out_name, size_t& out_next_offset) {
    out_name.clear();
    if (offset >= len) return false;

    size_t cur = offset;
    size_t jumped_next = 0;
    bool jumped = false;
    int hops = 0;

    while (cur < len) {
        uint8_t tag = pkt[cur];
        if (tag == 0) {
            if (!jumped) out_next_offset = cur + 1;
            else out_next_offset = jumped_next;
            if (out_name.empty()) out_name = ".";
            return true;
        }
        if ((tag & 0xC0) == 0xC0) {
            if (cur + 1 >= len) return false;
            uint16_t ptr = static_cast<uint16_t>(((tag & 0x3F) << 8) | pkt[cur + 1]);
            if (ptr >= len) return false;
            if (!jumped) {
                jumped_next = cur + 2;
                jumped = true;
            }
            cur = ptr;
            if (++hops > 16) return false;
            continue;
        }
        if ((tag & 0xC0) != 0) {
            return false;  // reserved label type
        }
        size_t label_len = tag;
        ++cur;
        if (cur + label_len > len || out_name.size() + label_len + 1 > 255) {
            return false;
        }
        if (!out_name.empty()) out_name.push_back('.');
        for (size_t i = 0; i < label_len; ++i) {
            char ch = static_cast<char>(pkt[cur + i]);
            out_name.push_back((ch >= 0x20 && ch < 0x7F) ? ch : '?');
        }
        cur += label_len;
    }
    return false;
}

std::string formatIpv6(const uint8_t* b) {
    uint16_t g[8];
    for (int i = 0; i < 8; ++i) {
        g[i] = readU16(b + i * 2);
    }
    int best_start = -1, best_len = 0;
    for (int i = 0; i < 8;) {
        if (g[i] != 0) {
            ++i;
            continue;
        }
        int j = i;
        while (j < 8 && g[j] == 0) ++j;
        if (j - i > best_len && j - i >= 2) {
            best_start = i;
            best_len = j - i;
        }
        i = j;
    }
    std::string out;
    for (int i = 0; i < 8;) {
        if (i == best_start) {
            out += (i == 0) ? "::" : ":";
            i += best_len;
            continue;
        }
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%x", g[i]);
        out += buf;
        if (i + 1 < 8 && i + 1 != best_start) out += ':';
        ++i;
    }
    return out;
}

}  // namespace

const char* typeName(uint16_t qtype) {
    switch (qtype) {
        case kTypeA: return "A";
        case kTypeNs: return "NS";
        case kTypeCname: return "CNAME";
        case kTypeSoa: return "SOA";
        case kTypePtr: return "PTR";
        case kTypeMx: return "MX";
        case kTypeTxt: return "TXT";
        case kTypeAaaa: return "AAAA";
        default: return "TYPE";
    }
}

const char* rcodeName(uint8_t rcode) {
    switch (rcode) {
        case 0: return "NOERROR";
        case 1: return "FORMERR";
        case 2: return "SERVFAIL";
        case 3: return "NXDOMAIN";
        case 4: return "NOTIMP";
        case 5: return "REFUSED";
        default: return "ERR";
    }
}

bool buildQuery(uint16_t txid, const std::string& domain, uint16_t qtype, std::vector<uint8_t>& out) {
    out.clear();
    std::string trimmed = domain;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '.')) trimmed.pop_back();
    if (trimmed.empty() || trimmed.size() > 253) return false;

    out.reserve(32 + trimmed.size());
    writeU16(out, txid);
    writeU16(out, 0x0100);  // Standard query, RD=1
    writeU16(out, 1);       // QDCOUNT
    writeU16(out, 0);       // ANCOUNT
    writeU16(out, 0);       // NSCOUNT
    writeU16(out, 0);       // ARCOUNT

    size_t pos = 0;
    while (pos < trimmed.size()) {
        size_t dot = trimmed.find('.', pos);
        if (dot == std::string::npos) dot = trimmed.size();
        size_t label_len = dot - pos;
        if (label_len == 0 || label_len > 63) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<uint8_t>(label_len));
        for (size_t i = pos; i < dot; ++i) {
            char c = trimmed[i];
            if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F) {
                out.clear();
                return false;
            }
            out.push_back(static_cast<uint8_t>(c));
        }
        pos = dot + 1;
    }
    out.push_back(0);
    writeU16(out, qtype);
    writeU16(out, 1);  // IN
    return true;
}

DnsResponse parseResponse(const uint8_t* pkt, size_t len, uint16_t expected_txid) {
    DnsResponse res;
    if (!pkt || len < 12) {
        res.error = "Short DNS packet";
        return res;
    }
    uint16_t txid = readU16(pkt);
    if (txid != expected_txid) {
        res.error = "TXID mismatch";
        return res;
    }
    uint16_t flags = readU16(pkt + 2);
    if ((flags & 0x8000U) == 0) {
        res.error = "Not a DNS response";
        return res;
    }
    res.rcode = static_cast<uint8_t>(flags & 0x0FU);
    uint16_t qdcount = readU16(pkt + 4);
    uint16_t ancount = readU16(pkt + 6);

    size_t off = 12;
    for (uint16_t q = 0; q < qdcount; ++q) {
        std::string qn;
        if (!decodeName(pkt, len, off, qn, off) || off + 4 > len) {
            res.error = "Malformed question";
            return res;
        }
        off += 4;
    }

    for (uint16_t a = 0; a < ancount; ++a) {
        DnsRecord rec;
        if (!decodeName(pkt, len, off, rec.name, off) || off + 10 > len) {
            res.error = "Truncated answer";
            return res;
        }
        rec.type = readU16(pkt + off);
        // uint16_t rclass = readU16(pkt + off + 2);
        rec.ttl = readU32(pkt + off + 4);
        uint16_t rdlen = readU16(pkt + off + 8);
        off += 10;
        if (off + rdlen > len) {
            res.error = "Truncated rdata";
            return res;
        }

        if (rec.type == kTypeA && rdlen == 4) {
            char buf[20];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                          pkt[off], pkt[off + 1], pkt[off + 2], pkt[off + 3]);
            rec.rdata = buf;
        } else if (rec.type == kTypeAaaa && rdlen == 16) {
            rec.rdata = formatIpv6(pkt + off);
        } else if (rec.type == kTypeCname || rec.type == kTypeNs || rec.type == kTypePtr) {
            size_t dummy = 0;
            if (!decodeName(pkt, len, off, rec.rdata, dummy)) {
                rec.rdata = "<bad name>";
            }
        } else if (rec.type == kTypeMx && rdlen >= 3) {
            uint16_t pref = readU16(pkt + off);
            std::string exch;
            size_t dummy = 0;
            if (decodeName(pkt, len, off + 2, exch, dummy)) {
                rec.rdata = std::to_string(pref) + " " + exch;
            } else {
                rec.rdata = "<bad mx>";
            }
        } else if (rec.type == kTypeTxt) {
            size_t p = off;
            size_t end = off + rdlen;
            while (p < end) {
                uint8_t slen = pkt[p++];
                size_t take = std::min<size_t>(slen, end - p);
                for (size_t k = 0; k < take; ++k) {
                    char c = static_cast<char>(pkt[p + k]);
                    rec.rdata.push_back((c >= 0x20 && c < 0x7F) ? c : '.');
                }
                p += take;
            }
        } else if (rec.type == kTypeSoa) {
            std::string mname, rname;
            size_t p2 = 0, dummy = 0;
            if (decodeName(pkt, len, off, mname, p2) &&
                p2 < off + rdlen &&
                decodeName(pkt, len, p2, rname, dummy)) {
                rec.rdata = mname + " (" + rname + ")";
            } else {
                rec.rdata = "<soa " + std::to_string(rdlen) + "B>";
            }
        } else {
            rec.rdata = "<" + std::to_string(rdlen) + "B>";
        }

        off += rdlen;
        res.answers.push_back(std::move(rec));
    }

    res.ok = true;
    return res;
}

uint16_t checksum16(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    size_t i = 0;
    while (i + 1 < len) {
        sum += (static_cast<uint32_t>(data[i]) << 8) | static_cast<uint32_t>(data[i + 1]);
        i += 2;
    }
    if (i < len) {
        sum += static_cast<uint32_t>(data[i]) << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum & 0xFFFFU);
}

}  // namespace dns_wire
