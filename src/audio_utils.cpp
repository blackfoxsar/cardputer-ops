#include "audio_utils.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

std::string toLowerCopy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string basenameOnly(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

bool endsWithAscii(const std::string& s, const char* suffix) {
    size_t slen = std::strlen(suffix);
    return s.size() >= slen && s.compare(s.size() - slen, slen, suffix) == 0;
}

}  // namespace

namespace audio_utils {

WavHeader makePcmWavHeader(uint32_t sample_rate, uint16_t channels,
                           uint16_t bits_per_sample, uint32_t data_bytes) {
    WavHeader h{};
    std::memcpy(h.riff, "RIFF", 4);
    h.file_size = 36U + data_bytes;
    std::memcpy(h.wave, "WAVE", 4);
    std::memcpy(h.fmt, "fmt ", 4);
    h.fmt_size = 16;
    h.audio_format = 1;
    h.num_channels = channels;
    h.sample_rate = sample_rate;
    uint16_t align = static_cast<uint16_t>(channels * (bits_per_sample / 8));
    h.block_align = align;
    h.byte_rate = sample_rate * align;
    h.bits_per_sample = bits_per_sample;
    std::memcpy(h.data, "data", 4);
    h.data_size = data_bytes;
    return h;
}

bool isMusicFilename(const std::string& name) {
    std::string base = basenameOnly(name);
    if (base.empty() || base[0] == '.') return false;
    std::string low = toLowerCopy(base);
    return endsWithAscii(low, ".mp3") || endsWithAscii(low, ".wav");
}

bool isWavFilename(const std::string& name) {
    std::string base = basenameOnly(name);
    if (base.empty() || base[0] == '.') return false;
    std::string low = toLowerCopy(base);
    return endsWithAscii(low, ".wav");
}

std::string formatMmSs(uint32_t total_sec) {
    uint32_t mm = total_sec / 60;
    uint32_t ss = total_sec % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02lu:%02lu",
                  static_cast<unsigned long>(mm),
                  static_cast<unsigned long>(ss));
    return buf;
}

}  // namespace audio_utils
