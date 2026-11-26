#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace dns_wire {

constexpr uint16_t kTypeA = 1;
constexpr uint16_t kTypeNs = 2;
constexpr uint16_t kTypeCname = 5;
constexpr uint16_t kTypeSoa = 6;
constexpr uint16_t kTypePtr = 12;
constexpr uint16_t kTypeMx = 15;
constexpr uint16_t kTypeTxt = 16;
constexpr uint16_t kTypeAaaa = 28;

struct DnsRecord {
    std::string name;
    uint16_t type = 0;
    uint32_t ttl = 0;
    std::string rdata;
};

struct DnsResponse {
    bool ok = false;
    uint8_t rcode = 0;
    std::string error;
    std::vector<DnsRecord> answers;
};

const char* typeName(uint16_t qtype);
const char* rcodeName(uint8_t rcode);

// Build a standard recursive DNS query packet (UDP payload) for domain + qtype.
bool buildQuery(uint16_t txid, const std::string& domain, uint16_t qtype, std::vector<uint8_t>& out);

// Parse a DNS UDP response packet and extract answer records safely.
DnsResponse parseResponse(const uint8_t* pkt, size_t len, uint16_t expected_txid);

// RFC 1071 16-bit Internet checksum (big-endian result ready for IP/ICMP headers).
uint16_t checksum16(const uint8_t* data, size_t len);

}  // namespace dns_wire
