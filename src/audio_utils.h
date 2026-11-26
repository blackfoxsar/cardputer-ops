#pragma once

#include <cstdint>
#include <string>

namespace audio_utils {

struct __attribute__((packed)) WavHeader {
    char riff[4];
    uint32_t file_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
};

static_assert(sizeof(WavHeader) == 44, "WavHeader must be 44 bytes");

WavHeader makePcmWavHeader(uint32_t sample_rate, uint16_t channels,
                           uint16_t bits_per_sample, uint32_t data_bytes);
bool isMusicFilename(const std::string& name);
bool isWavFilename(const std::string& name);
std::string formatMmSs(uint32_t total_sec);

}  // namespace audio_utils
