#include "app_music.h"

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <driver/gpio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "app_alarm.h"
#include "audio_hw.h"
#include "audio_utils.h"
#include "config.h"
#include "third_party/dr_mp3.h"
#include "ui_common.h"

namespace {

struct MusicTrack {
    std::string name;
    std::string path;
    uint32_t size = 0;
};

struct SdMusicGuard {
    bool spi_started = false;
    bool mounted = false;

    bool mount() {
        SPI.begin(cfg::SD_SCK, cfg::SD_MISO, cfg::SD_MOSI, cfg::SD_CS);
        spi_started = true;
        mounted = SD.begin(cfg::SD_CS, SPI, 25000000);
        return mounted;
    }

    void unmount() {
        if (mounted) {
            SD.end();
            mounted = false;
        }
        if (spi_started) {
            SPI.end();
            spi_started = false;
        }
    }

    ~SdMusicGuard() {
        unmount();
    }
};

enum class RepeatMode {
    ALL = 0,
    ONE = 1,
    OFF = 2,
};

const char* repeatLabel(RepeatMode m) {
    switch (m) {
        case RepeatMode::ALL: return "ALL";
        case RepeatMode::ONE: return "ONE";
        default: return "OFF";
    }
}

void ensureMusicDir() {
    if (!SD.exists(cfg::SD_DIR_MUSIC)) {
        SD.mkdir(cfg::SD_DIR_MUSIC);
    }
}

std::vector<MusicTrack> scanMusicTracks() {
    std::vector<MusicTrack> tracks;
    File dir = SD.open(cfg::SD_DIR_MUSIC);
    if (!dir || !dir.isDirectory()) return tracks;

    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            std::string raw = f.name();
            size_t slash = raw.find_last_of('/');
            std::string base = (slash == std::string::npos) ? raw : raw.substr(slash + 1);
            if (audio_utils::isMusicFilename(base)) {
                tracks.push_back(MusicTrack{
                    base,
                    std::string(cfg::SD_DIR_MUSIC) + "/" + base,
                    static_cast<uint32_t>(f.size()),
                });
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    std::sort(tracks.begin(), tracks.end(), [](const MusicTrack& a, const MusicTrack& b) {
        return a.name < b.name;
    });
    return tracks;
}

void runTestTone1kHz() {
    app_alarm::SuppressGuard alarm_guard;
    int vol_pct = 50;
    if (!audio_hw::prepareSpeaker(vol_pct)) {
        ui::showToast("Speaker Error", "Failed to start ES8311", "", cfg::COL_RED);
        return;
    }

    static int16_t tone_buf[480];
    static bool tone_ready = false;
    if (!tone_ready) {
        constexpr double kTwoPi = 6.28318530717958647692;
        for (size_t i = 0; i < 480; ++i) {
            tone_buf[i] = static_cast<int16_t>(
                std::sin(kTwoPi * static_cast<double>(i) / 48.0) * 20000.0);
        }
        tone_ready = true;
    }

    bool muted = false;
    M5Cardputer.Speaker.playRaw(tone_buf, 480, 48000, false, UINT32_MAX, 0, true);

    ui::waitKeyRelease();
    bool redraw = true;

    while (true) {
        if (redraw) {
            redraw = false;

            auto& cv = ui::canvas();
            cv.fillScreen(cfg::COL_BG);
            ui::drawHeader("Audio Playback");

            cv.setTextColor(muted ? cfg::COL_YELLOW : cfg::COL_GREEN, cfg::COL_BG);
            cv.drawString(muted ? "[ MUTED ]" : "[ PLAYING 1 kHz ]", 8, 28);

            cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            cv.drawString("1000 Hz Sine Wave (48 kHz)", 8, 52);

            cv.setTextColor(cfg::COL_CYAN, cfg::COL_BG);
            cv.drawString(("Vol: " + std::to_string(vol_pct) + "%").c_str(), 8, 74);

            ui::drawHints("Space:Mute  ;.:Vol", "Enter / ESC:Back");
            cv.pushSprite(0, 0);
        }

        KeyInput k = ui::pollInput(65);
        ui::applyMenuNav(k);

        if (k.enter || k.esc || k.btn_g0) {
            break;
        }
        if (k.ch == ' ') {
            muted = !muted;
            if (muted) {
                M5Cardputer.Speaker.stop(0);
            } else {
                M5Cardputer.Speaker.playRaw(tone_buf, 480, 48000, false, UINT32_MAX, 0, true);
            }
            redraw = true;
        } else if (k.up || k.ch == '+' || k.ch == '=') {
            vol_pct = audio_utils::stepVolumePct(vol_pct, +1, k.repeat_count);
            M5Cardputer.Speaker.setVolume(audio_utils::volumePctToHardware(vol_pct));
            redraw = true;
        } else if (k.down || k.ch == '-') {
            vol_pct = audio_utils::stepVolumePct(vol_pct, -1, k.repeat_count);
            M5Cardputer.Speaker.setVolume(audio_utils::volumePctToHardware(vol_pct));
            redraw = true;
        }
        delay(20);
    }

    audio_hw::restoreQuietSpeakerIdle();
    ui::waitKeyRelease();
}

struct PlayerCtx {
    static constexpr size_t kBufCount = 3;
    static constexpr size_t kMaxSamplesPerBuf = 2304;  // 1152 stereo frames

    const std::vector<MusicTrack>* tracks = nullptr;
    SemaphoreHandle_t mtx = nullptr;
    SemaphoreHandle_t done = nullptr;

    int16_t pcm_buf[kBufCount][kMaxSamplesPerBuf]{};
    std::atomic<bool> pcm_busy[kBufCount] = {{false}, {false}, {false}};

    std::atomic<int> cur_idx{0};
    std::atomic<int> track_req{0};
    std::atomic<bool> paused{false};
    std::atomic<bool> quit{false};
    std::atomic<bool> abort_track{false};
    std::atomic<bool> active{false};
    std::atomic<int> repeat_mode{0};
    std::atomic<uint32_t> frames_played{0};
    std::atomic<uint32_t> cur_sample_rate{44100};

    std::string meta_file;
    std::string meta_title;
    std::string meta_artist;
    std::string meta_err;
};

void spkBufReleased(void* args, const void* data, uint8_t) {
    auto* ctx = static_cast<PlayerCtx*>(args);
    if (!ctx) return;
    for (size_t i = 0; i < PlayerCtx::kBufCount; ++i) {
        if (data == ctx->pcm_buf[i]) {
            ctx->pcm_busy[i].store(false, std::memory_order_release);
        }
    }
}

size_t mp3OnRead(void* pUserData, void* pBufferOut, size_t bytesToRead) {
    auto* f = static_cast<File*>(pUserData);
    if (!f || !*f) return 0;
    return f->read(static_cast<uint8_t*>(pBufferOut), bytesToRead);
}

drmp3_bool32 mp3OnSeek(void* pUserData, int offset, drmp3_seek_origin origin) {
    auto* f = static_cast<File*>(pUserData);
    if (!f || !*f) return DRMP3_FALSE;
    int64_t fsize = static_cast<int64_t>(f->size());
    int64_t target = 0;
    if (origin == DRMP3_SEEK_SET) {
        target = static_cast<int64_t>(offset);
    } else if (origin == DRMP3_SEEK_CUR) {
        target = static_cast<int64_t>(f->position()) + static_cast<int64_t>(offset);
    } else if (origin == DRMP3_SEEK_END) {
        target = fsize + static_cast<int64_t>(offset);
    } else {
        return DRMP3_FALSE;
    }
    if (target < 0) target = 0;
    if (target > fsize) target = fsize;
    return f->seek(static_cast<uint32_t>(target), SeekSet) ? DRMP3_TRUE : DRMP3_FALSE;
}

drmp3_bool32 mp3OnTell(void* pUserData, drmp3_int64* pCursor) {
    auto* f = static_cast<File*>(pUserData);
    if (!f || !*f || !pCursor) return DRMP3_FALSE;
    *pCursor = static_cast<drmp3_int64>(f->position());
    return DRMP3_TRUE;
}

std::string trimId3Ascii(const char* src, size_t len) {
    size_t actual = 0;
    while (actual < len && src[actual] != '\0') {
        ++actual;
    }
    while (actual > 0 && src[actual - 1] == ' ') {
        --actual;
    }
    std::string out;
    out.reserve(actual);
    for (size_t i = 0; i < actual; ++i) {
        unsigned char uc = static_cast<unsigned char>(src[i]);
        if (uc >= 0x20 && uc < 0x7F) {
            out.push_back(static_cast<char>(uc));
        } else if (uc >= 0x80) {
            out.push_back(static_cast<char>(uc));
        }
    }
    return out;
}

std::string decodeId3TextFrame(const uint8_t* data, size_t len) {
    if (len == 0) return "";
    uint8_t enc = data[0];
    const uint8_t* p = data + 1;
    size_t plen = len - 1;
    if (plen == 0) return "";

    if (enc == 0 || enc == 3) {
        return trimId3Ascii(reinterpret_cast<const char*>(p), plen);
    }

    if (enc == 1 || enc == 2) {
        bool little_endian = false;
        if (enc == 1 && plen >= 2) {
            if (p[0] == 0xFF && p[1] == 0xFE) {
                little_endian = true;
                p += 2;
                plen -= 2;
            } else if (p[0] == 0xFE && p[1] == 0xFF) {
                little_endian = false;
                p += 2;
                plen -= 2;
            }
        }
        std::string out;
        for (size_t i = 0; i + 1 < plen; i += 2) {
            uint16_t ch = little_endian
                              ? static_cast<uint16_t>(p[i] | (static_cast<uint16_t>(p[i + 1]) << 8))
                              : static_cast<uint16_t>((static_cast<uint16_t>(p[i]) << 8) | p[i + 1]);
            if (ch == 0) break;
            if (ch >= 0x20 && ch < 0x7F) {
                out.push_back(static_cast<char>(ch));
            } else if (ch >= 0x80) {
                out.push_back('?');
            }
        }
        return out;
    }

    return "";
}

void parseMp3Id3Metadata(File& f, std::string& out_title, std::string& out_artist) {
    out_title.clear();
    out_artist.clear();

    uint32_t fsize = static_cast<uint32_t>(f.size());
    f.seek(0, SeekSet);

    uint8_t hdr[10];
    if (fsize >= 10 && f.read(hdr, 10) == 10 && std::memcmp(hdr, "ID3", 3) == 0) {
        uint8_t ver_major = hdr[3];
        uint8_t flags = hdr[5];
        uint32_t tag_size = ((static_cast<uint32_t>(hdr[6]) & 0x7FU) << 21) |
                            ((static_cast<uint32_t>(hdr[7]) & 0x7FU) << 14) |
                            ((static_cast<uint32_t>(hdr[8]) & 0x7FU) << 7) |
                            (static_cast<uint32_t>(hdr[9]) & 0x7FU);
        uint32_t tag_end = std::min<uint32_t>(fsize, 10U + tag_size);

        if ((ver_major == 3 || ver_major == 4) && (flags & 0x40U)) {
            uint8_t ext[4];
            if (f.position() + 4 <= tag_end && f.read(ext, 4) == 4) {
                uint32_t ext_sz = (ver_major == 4)
                                      ? (((static_cast<uint32_t>(ext[0]) & 0x7FU) << 21) |
                                         ((static_cast<uint32_t>(ext[1]) & 0x7FU) << 14) |
                                         ((static_cast<uint32_t>(ext[2]) & 0x7FU) << 7) |
                                         (static_cast<uint32_t>(ext[3]) & 0x7FU))
                                      : ((static_cast<uint32_t>(ext[0]) << 24) |
                                         (static_cast<uint32_t>(ext[1]) << 16) |
                                         (static_cast<uint32_t>(ext[2]) << 8) |
                                         static_cast<uint32_t>(ext[3]));
                uint32_t skip = (ver_major == 4 && ext_sz >= 4) ? (ext_sz - 4) : ext_sz;
                uint32_t cur = static_cast<uint32_t>(f.position());
                uint32_t avail = (tag_end > cur) ? (tag_end - cur) : 0;
                uint32_t next_pos = (skip >= avail) ? tag_end : (cur + skip);
                f.seek(next_pos, SeekSet);
            }
        }

        while ((ver_major == 3 || ver_major == 4) &&
               static_cast<uint32_t>(f.position()) + 10 <= tag_end &&
               (out_title.empty() || out_artist.empty())) {
            uint8_t fhdr[10];
            if (f.read(fhdr, 10) != 10) break;
            if (fhdr[0] == 0) break;

            uint32_t frame_sz = 0;
            if (ver_major == 4) {
                frame_sz = ((static_cast<uint32_t>(fhdr[4]) & 0x7FU) << 21) |
                           ((static_cast<uint32_t>(fhdr[5]) & 0x7FU) << 14) |
                           ((static_cast<uint32_t>(fhdr[6]) & 0x7FU) << 7) |
                           (static_cast<uint32_t>(fhdr[7]) & 0x7FU);
            } else {
                frame_sz = (static_cast<uint32_t>(fhdr[4]) << 24) |
                           (static_cast<uint32_t>(fhdr[5]) << 16) |
                           (static_cast<uint32_t>(fhdr[6]) << 8) |
                           static_cast<uint32_t>(fhdr[7]);
            }

            uint32_t cur_pos = static_cast<uint32_t>(f.position());
            if (frame_sz == 0 || cur_pos > tag_end || frame_sz > (tag_end - cur_pos)) break;

            bool is_title = (std::memcmp(fhdr, "TIT2", 4) == 0);
            bool is_artist = (std::memcmp(fhdr, "TPE1", 4) == 0);
            if (is_title || is_artist) {
                uint8_t payload[256];
                size_t to_read = std::min<size_t>(frame_sz, sizeof(payload));
                size_t rd = f.read(payload, to_read);
                std::string text = decodeId3TextFrame(payload, rd);
                if (is_title && out_title.empty()) out_title = text;
                if (is_artist && out_artist.empty()) out_artist = text;
                if (frame_sz > rd) {
                    f.seek(cur_pos + frame_sz, SeekSet);
                }
            } else {
                f.seek(cur_pos + frame_sz, SeekSet);
            }
        }
    }

    if ((out_title.empty() || out_artist.empty()) && fsize >= 128) {
        uint8_t tag[128];
        if (f.seek(fsize - 128, SeekSet) && f.read(tag, 128) == 128 &&
            std::memcmp(tag, "TAG", 3) == 0) {
            if (out_title.empty()) {
                out_title = trimId3Ascii(reinterpret_cast<const char*>(tag + 3), 30);
            }
            if (out_artist.empty()) {
                out_artist = trimId3Ascii(reinterpret_cast<const char*>(tag + 33), 30);
            }
        }
    }

    f.seek(0, SeekSet);
}

struct __attribute__((packed)) WavFmtChunk {
    uint16_t audio_fmt;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_per_sec;
    uint16_t block_size;
    uint16_t bits_per_sample;
};

struct __attribute__((packed)) WavChunkHdr {
    char id[4];
    uint32_t size;
};

struct WavStreamState {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t remaining = 0;
};

bool openWavStream(File& f, WavStreamState& st) {
    st = WavStreamState{};
    char riff[12];
    if (f.read(reinterpret_cast<uint8_t*>(riff), 12) != 12 ||
        std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }

    WavFmtChunk fmt{};
    bool got_fmt = false;
    bool got_data = false;

    WavChunkHdr chk{};
    while (f.read(reinterpret_cast<uint8_t*>(&chk), sizeof(chk)) == sizeof(chk)) {
        if (std::memcmp(chk.id, "fmt ", 4) == 0 && chk.size >= sizeof(WavFmtChunk)) {
            if (f.read(reinterpret_cast<uint8_t*>(&fmt), sizeof(fmt)) != sizeof(fmt)) break;
            if (chk.size > sizeof(fmt)) {
                uint32_t extra = chk.size - static_cast<uint32_t>(sizeof(fmt));
                if (!f.seek(extra + (chk.size & 1U), SeekCur)) break;
            }
            got_fmt = true;
        } else if (std::memcmp(chk.id, "data", 4) == 0) {
            st.remaining = chk.size;
            got_data = true;
            break;
        } else {
            uint32_t skip = chk.size + (chk.size & 1U);
            if (!f.seek(skip, SeekCur)) break;
        }
    }

    if (!got_fmt || !got_data || fmt.audio_fmt != 1 || fmt.sample_rate == 0 ||
        (fmt.channels != 1 && fmt.channels != 2) ||
        (fmt.bits_per_sample != 8 && fmt.bits_per_sample != 16)) {
        return false;
    }

    st.sample_rate = fmt.sample_rate;
    st.channels = fmt.channels;
    st.bits_per_sample = fmt.bits_per_sample;
    return true;
}

void audioDecodeTask(void* arg) {
    auto* ctx = static_cast<PlayerCtx*>(arg);
    drmp3* mp3 = new drmp3();
    File cur_file;
    bool mp3_open = false;
    bool wav_open = false;
    WavStreamState wav{};
    size_t buf_idx = 0;

    auto stopCurrent = [&](bool abort_speaker) {
        if (abort_speaker) {
            M5Cardputer.Speaker.stop(0);
            uint32_t t0 = millis();
            while (M5Cardputer.Speaker.isPlaying(0) && (millis() - t0 < 500)) {
                vTaskDelay(1);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            for (size_t i = 0; i < PlayerCtx::kBufCount; ++i) {
                ctx->pcm_busy[i].store(false, std::memory_order_release);
            }
        } else {
            uint32_t t0 = millis();
            for (size_t i = 0; i < PlayerCtx::kBufCount; ++i) {
                while (ctx->pcm_busy[i].load(std::memory_order_acquire) && (millis() - t0 < 1500)) {
                    if (ctx->quit.load(std::memory_order_acquire) ||
                        ctx->abort_track.load(std::memory_order_acquire) ||
                        ctx->track_req.load(std::memory_order_acquire) != 0) {
                        break;
                    }
                    vTaskDelay(1);
                }
            }
        }
        if (mp3_open) {
            drmp3_uninit(mp3);
            mp3_open = false;
        }
        wav_open = false;
        if (cur_file) {
            cur_file.close();
        }
        ctx->active.store(false, std::memory_order_release);
    };

    auto startTrack = [&](int index, bool abort_prev) -> bool {
        stopCurrent(abort_prev);
        ctx->abort_track.store(false, std::memory_order_release);

        int n = static_cast<int>(ctx->tracks->size());
        if (n == 0) return false;
        index = ((index % n) + n) % n;
        ctx->cur_idx.store(index, std::memory_order_release);
        const auto& tr = (*ctx->tracks)[index];

        xSemaphoreTake(ctx->mtx, portMAX_DELAY);
        ctx->meta_file = tr.name;
        ctx->meta_title.clear();
        ctx->meta_artist.clear();
        ctx->meta_err.clear();
        xSemaphoreGive(ctx->mtx);

        ctx->frames_played.store(0, std::memory_order_release);

        cur_file = SD.open(tr.path.c_str(), FILE_READ);
        if (!cur_file) {
            xSemaphoreTake(ctx->mtx, portMAX_DELAY);
            ctx->meta_err = "Cannot open file";
            xSemaphoreGive(ctx->mtx);
            return false;
        }

        if (audio_utils::isWavFilename(tr.name)) {
            if (!openWavStream(cur_file, wav)) {
                cur_file.close();
                xSemaphoreTake(ctx->mtx, portMAX_DELAY);
                ctx->meta_err = "Unsupported WAV";
                xSemaphoreGive(ctx->mtx);
                return false;
            }
            wav_open = true;
            ctx->cur_sample_rate.store(wav.sample_rate, std::memory_order_release);
            ctx->active.store(true, std::memory_order_release);
            return true;
        }

        std::string title, artist;
        parseMp3Id3Metadata(cur_file, title, artist);
        if (!title.empty() || !artist.empty()) {
            xSemaphoreTake(ctx->mtx, portMAX_DELAY);
            ctx->meta_title = title;
            ctx->meta_artist = artist;
            xSemaphoreGive(ctx->mtx);
        }

        bool inited = drmp3_init(mp3, mp3OnRead, mp3OnSeek, mp3OnTell, nullptr, &cur_file, nullptr);
        if (!inited || mp3->channels == 0 || mp3->channels > 2 || mp3->sampleRate == 0) {
            if (inited) {
                drmp3_uninit(mp3);
            }
            cur_file.close();
            xSemaphoreTake(ctx->mtx, portMAX_DELAY);
            ctx->meta_err = "MP3 decode error";
            xSemaphoreGive(ctx->mtx);
            return false;
        }

        mp3_open = true;
        ctx->cur_sample_rate.store(mp3->sampleRate, std::memory_order_release);
        ctx->active.store(true, std::memory_order_release);
        return true;
    };

    auto advanceTrack = [&](int step, bool abort_prev) {
        int n = static_cast<int>(ctx->tracks->size());
        int base = ctx->cur_idx.load(std::memory_order_acquire);
        for (int tries = 0; tries < n && !ctx->quit.load(std::memory_order_acquire); ++tries) {
            int nxt = (((base + step) % n) + n) % n;
            if (startTrack(nxt, abort_prev && tries == 0)) return;
            base = nxt;
            step = (step >= 0) ? 1 : -1;
        }
    };

    auto waitFreeBuf = [&](size_t idx) -> bool {
        while (ctx->pcm_busy[idx].load(std::memory_order_acquire)) {
            if (ctx->quit.load(std::memory_order_acquire) ||
                ctx->abort_track.load(std::memory_order_acquire) ||
                ctx->track_req.load(std::memory_order_acquire) != 0) {
                return false;
            }
            vTaskDelay(1);
        }
        return true;
    };

    startTrack(ctx->cur_idx.load(std::memory_order_acquire), false);

    while (!ctx->quit.load(std::memory_order_acquire)) {
        int step = ctx->track_req.exchange(0);
        if (step != 0) {
            ctx->paused.store(false, std::memory_order_release);
            advanceTrack(step, true);
            continue;
        }

        if (ctx->paused.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        if (!mp3_open && !wav_open) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!waitFreeBuf(buf_idx)) {
            continue;
        }

        uint32_t frames = 0;
        size_t samples = 0;
        uint32_t rate = 44100;
        bool stereo = false;
        bool eof = false;

        if (mp3_open) {
            uint64_t max_frames = PlayerCtx::kMaxSamplesPerBuf / mp3->channels;
            if (max_frames > 1152) max_frames = 1152;
            drmp3_uint64 got = drmp3_read_pcm_frames_s16(mp3, max_frames, ctx->pcm_buf[buf_idx]);
            if (got == 0) {
                eof = true;
            } else {
                frames = static_cast<uint32_t>(got);
                samples = static_cast<size_t>(got) * mp3->channels;
                rate = mp3->sampleRate;
                stereo = (mp3->channels == 2);
                ctx->cur_sample_rate.store(rate, std::memory_order_release);
            }
        } else {
            if (wav.remaining == 0) {
                eof = true;
            } else if (wav.bits_per_sample == 16) {
                uint32_t align = wav.channels * 2U;
                size_t max_bytes = std::min<size_t>(wav.remaining, PlayerCtx::kMaxSamplesPerBuf * sizeof(int16_t));
                max_bytes -= (max_bytes % align);
                size_t rd = (max_bytes > 0)
                                ? cur_file.read(reinterpret_cast<uint8_t*>(ctx->pcm_buf[buf_idx]), max_bytes)
                                : 0;
                rd -= (rd % align);
                if (rd == 0) {
                    eof = true;
                } else {
                    wav.remaining -= static_cast<uint32_t>(rd);
                    samples = rd / sizeof(int16_t);
                    frames = static_cast<uint32_t>(rd / align);
                    rate = wav.sample_rate;
                    stereo = (wav.channels == 2);
                }
            } else {
                uint8_t raw8[1024];
                uint32_t align = wav.channels;
                size_t max_bytes = std::min<size_t>(wav.remaining, sizeof(raw8));
                max_bytes -= (max_bytes % align);
                size_t rd = (max_bytes > 0) ? cur_file.read(raw8, max_bytes) : 0;
                rd -= (rd % align);
                if (rd == 0) {
                    eof = true;
                } else {
                    wav.remaining -= static_cast<uint32_t>(rd);
                    for (size_t i = 0; i < rd; ++i) {
                        ctx->pcm_buf[buf_idx][i] =
                            static_cast<int16_t>((static_cast<int>(raw8[i]) - 128) << 8);
                    }
                    samples = rd;
                    frames = static_cast<uint32_t>(rd / align);
                    rate = wav.sample_rate;
                    stereo = (wav.channels == 2);
                }
            }
        }

        if (eof) {
            int rm = ctx->repeat_mode.load(std::memory_order_acquire);
            if (rm == static_cast<int>(RepeatMode::ONE)) {
                startTrack(ctx->cur_idx.load(std::memory_order_acquire), false);
            } else if (rm == static_cast<int>(RepeatMode::ALL)) {
                advanceTrack(1, false);
            } else {
                int nxt = ctx->cur_idx.load(std::memory_order_acquire) + 1;
                if (nxt < static_cast<int>(ctx->tracks->size())) {
                    advanceTrack(1, false);
                } else {
                    stopCurrent(false);
                    xSemaphoreTake(ctx->mtx, portMAX_DELAY);
                    ctx->meta_err = "Playback finished";
                    xSemaphoreGive(ctx->mtx);
                }
            }
            continue;
        }

        while (ctx->paused.load(std::memory_order_acquire)) {
            if (ctx->quit.load(std::memory_order_acquire) ||
                ctx->abort_track.load(std::memory_order_acquire) ||
                ctx->track_req.load(std::memory_order_acquire) != 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(15));
        }

        if (ctx->quit.load(std::memory_order_acquire) ||
            ctx->abort_track.load(std::memory_order_acquire) ||
            ctx->track_req.load(std::memory_order_acquire) != 0) {
            continue;
        }

        ctx->pcm_busy[buf_idx].store(true, std::memory_order_release);
        bool queued = M5Cardputer.Speaker.playRaw(
            ctx->pcm_buf[buf_idx], samples, rate, stereo, 1, 0, false);
        if (queued) {
            ctx->frames_played.fetch_add(frames, std::memory_order_relaxed);
            buf_idx = (buf_idx + 1) % PlayerCtx::kBufCount;
        } else {
            ctx->pcm_busy[buf_idx].store(false, std::memory_order_release);
            vTaskDelay(1);
        }
    }

    ctx->abort_track.store(true, std::memory_order_release);
    stopCurrent(true);
    delete mp3;
    xSemaphoreGive(ctx->done);
    vTaskDelete(nullptr);
}

bool runNowPlaying(const std::vector<MusicTrack>& tracks, int& io_cursor) {
    app_alarm::SuppressGuard alarm_guard;

    auto ctx = std::unique_ptr<PlayerCtx>(new PlayerCtx());
    ctx->tracks = &tracks;
    ctx->mtx = xSemaphoreCreateMutex();
    ctx->done = xSemaphoreCreateBinary();
    ctx->cur_idx.store(io_cursor, std::memory_order_release);

    M5Cardputer.Speaker.setBufferReleaseCallback(ctx.get(), spkBufReleased);

    int vol_pct = 65;
    if (!audio_hw::prepareSpeaker(vol_pct)) {
        M5Cardputer.Speaker.setBufferReleaseCallback(nullptr, nullptr);
        vSemaphoreDelete(ctx->mtx);
        vSemaphoreDelete(ctx->done);
        ui::showToast("Speaker Error", "Failed to start ES8311", "", cfg::COL_RED);
        return true;
    }

    TaskHandle_t task_h = nullptr;
    BaseType_t ok = xTaskCreate(audioDecodeTask, "audio_dec", 12288, ctx.get(), 2, &task_h);
    if (ok != pdPASS) {
        M5Cardputer.Speaker.setBufferReleaseCallback(nullptr, nullptr);
        audio_hw::restoreQuietSpeakerIdle();
        vSemaphoreDelete(ctx->mtx);
        vSemaphoreDelete(ctx->done);
        ui::showToast("Memory Error", "Failed to start decoder task", "", cfg::COL_RED);
        return true;
    }

    ui::waitKeyRelease();
    bool back_to_list = true;
    uint32_t last_draw = 0;

    while (true) {
        uint32_t now = millis();
        if (now - last_draw >= 120) {
            last_draw = now;

            std::string file_s, title_s, artist_s, err_s;
            xSemaphoreTake(ctx->mtx, portMAX_DELAY);
            file_s = ctx->meta_file;
            title_s = ctx->meta_title;
            artist_s = ctx->meta_artist;
            err_s = ctx->meta_err;
            xSemaphoreGive(ctx->mtx);

            int idx = ctx->cur_idx.load(std::memory_order_acquire);
            bool paused = ctx->paused.load(std::memory_order_acquire);
            bool playing = ctx->active.load(std::memory_order_acquire);
            RepeatMode rm = static_cast<RepeatMode>(ctx->repeat_mode.load(std::memory_order_acquire));

            uint32_t rate = ctx->cur_sample_rate.load(std::memory_order_acquire);
            uint32_t frames = ctx->frames_played.load(std::memory_order_acquire);
            uint32_t sec = rate ? (frames / rate) : 0;

            auto& cv = ui::canvas();
            cv.fillScreen(cfg::COL_BG);
            ui::drawHeader("Audio Playback");

            std::string badge = paused ? "PAUSED" : (playing ? "PLAYING" : "STOPPED");
            cv.setTextColor(paused ? cfg::COL_YELLOW : (playing ? cfg::COL_GREEN : cfg::COL_GRAY), cfg::COL_BG);
            cv.drawString((badge + "  [" + std::to_string(idx + 1) + "/" + std::to_string(tracks.size()) + "]").c_str(),
                          8, 25);

            cv.setTextDatum(top_right);
            cv.setTextColor(cfg::COL_CYAN, cfg::COL_BG);
            cv.drawString(("R:" + std::string(repeatLabel(rm)) + " V:" + std::to_string(vol_pct) + "%").c_str(),
                          232, 25);
            cv.setTextDatum(top_left);

            std::string main_line = title_s.empty() ? file_s : title_s;
            cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            cv.drawString(ui::fitCols(main_line, 37).c_str(), 8, 44);

            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            if (!err_s.empty()) {
                cv.setTextColor(cfg::COL_RED, cfg::COL_BG);
                cv.drawString(ui::fitCols(err_s, 37).c_str(), 8, 62);
            } else if (!artist_s.empty()) {
                cv.drawString(ui::fitCols(artist_s, 37).c_str(), 8, 62);
            } else {
                cv.drawString(ui::fitCols(file_s, 37).c_str(), 8, 62);
            }

            cv.setTextColor(cfg::COL_ORANGE, cfg::COL_BG);
            cv.drawString(("Elapsed: " + audio_utils::formatMmSs(sec)).c_str(), 8, 84);

            ui::drawHints("Space:Pause ,/:Prev/Nxt", ";.:Vol M:Rep ESC:List");
            cv.pushSprite(0, 0);
        }

        KeyInput k = ui::pollInput(65);
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0 || k.enter || lc == 'l') {
            back_to_list = true;
            break;
        }
        if (k.ch == ' ') {
            ctx->paused.store(!ctx->paused.load(std::memory_order_acquire), std::memory_order_release);
            last_draw = 0;
        } else if (k.left) {
            ctx->abort_track.store(true, std::memory_order_release);
            M5Cardputer.Speaker.stop(0);
            ctx->track_req.fetch_sub(1);
            last_draw = 0;
        } else if (k.right) {
            ctx->abort_track.store(true, std::memory_order_release);
            M5Cardputer.Speaker.stop(0);
            ctx->track_req.fetch_add(1);
            last_draw = 0;
        } else if (k.up || k.ch == '+' || k.ch == '=') {
            vol_pct = audio_utils::stepVolumePct(vol_pct, +1, k.repeat_count);
            M5Cardputer.Speaker.setVolume(audio_utils::volumePctToHardware(vol_pct));
            last_draw = 0;
        } else if (k.down || k.ch == '-') {
            vol_pct = audio_utils::stepVolumePct(vol_pct, -1, k.repeat_count);
            M5Cardputer.Speaker.setVolume(audio_utils::volumePctToHardware(vol_pct));
            last_draw = 0;
        } else if (lc == 'm') {
            int rm = (ctx->repeat_mode.load(std::memory_order_acquire) + 1) % 3;
            ctx->repeat_mode.store(rm, std::memory_order_release);
            last_draw = 0;
        }
        delay(20);
    }

    io_cursor = ctx->cur_idx.load(std::memory_order_acquire);
    ctx->quit.store(true, std::memory_order_release);
    ctx->abort_track.store(true, std::memory_order_release);
    M5Cardputer.Speaker.stop(0);
    xSemaphoreTake(ctx->done, pdMS_TO_TICKS(3000));
    M5Cardputer.Speaker.setBufferReleaseCallback(nullptr, nullptr);
    audio_hw::restoreQuietSpeakerIdle();
    vSemaphoreDelete(ctx->mtx);
    vSemaphoreDelete(ctx->done);
    ui::waitKeyRelease();
    return back_to_list;
}

}  // namespace

void runMusicApp() {
    ui::waitKeyRelease();
    SdMusicGuard sd;
    std::vector<MusicTrack> tracks;
    if (sd.mount()) {
        ensureMusicDir();
        tracks = scanMusicTracks();
    }

    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 5;

    while (true) {
        int total_rows = 1 + static_cast<int>(tracks.size());

        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Audio Playback");

        for (int i = 0; i < kMaxVis && (scroll + i) < total_rows; ++i) {
            int row = scroll + i;
            int y = 26 + i * 18;
            bool sel = (row == cursor);
            if (sel) {
                cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(row == 0 ? cfg::COL_CYAN : cfg::COL_CREAM, cfg::COL_BG);
            }

            if (row == 0) {
                cv.drawString("[ 1 kHz Test Tone ]", 8, y + 2);
                if (!sel) cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
                cv.setTextDatum(top_right);
                cv.drawString("BUILTIN", 232, y + 2);
                cv.setTextDatum(top_left);
            } else {
                const auto& tr = tracks[row - 1];
                cv.drawString(ui::fitCols(tr.name, 29).c_str(), 8, y + 2);
                if (!sel) cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
                cv.setTextDatum(top_right);
                uint32_t mb10 = (tr.size * 10UL) / (1024UL * 1024UL);
                std::string sz_s = (tr.size >= 1024UL * 1024UL)
                                       ? (std::to_string(mb10 / 10) + "." + std::to_string(mb10 % 10) + "M")
                                       : (std::to_string(std::max<uint32_t>(1, tr.size / 1024)) + "K");
                cv.drawString(sz_s.c_str(), 232, y + 2);
                cv.setTextDatum(top_left);
            }
        }

        if (!sd.mounted) {
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString("No SD card (1 kHz tone ready)", 8, 56);
        } else if (tracks.empty()) {
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.drawString("No .mp3 / .wav in /music/", 8, 56);
        }

        ui::drawHints("Enter play", "R rescan  ESC back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0) {
            ui::waitKeyRelease();
            return;
        }
        if (lc == 'r') {
            sd.unmount();
            tracks.clear();
            if (sd.mount()) {
                ensureMusicDir();
                tracks = scanMusicTracks();
            }
            cursor = 0;
            scroll = 0;
        } else if (k.up) {
            cursor = (cursor - 1 + total_rows) % total_rows;
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, total_rows - kMaxVis);
        } else if (k.down) {
            cursor = (cursor + 1) % total_rows;
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (k.enter) {
            if (cursor == 0) {
                runTestTone1kHz();
            } else {
                int track_idx = cursor - 1;
                runNowPlaying(tracks, track_idx);
                cursor = track_idx + 1;
                if (cursor < scroll) scroll = cursor;
                if (cursor >= scroll + kMaxVis) scroll = std::max(0, cursor - kMaxVis + 1);
            }
        }
        delay(25);
    }
}
