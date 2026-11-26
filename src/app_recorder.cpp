#include "app_recorder.h"

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <driver/gpio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

#include "audio_utils.h"
#include "config.h"
#include "time_sync.h"
#include "ui_common.h"

namespace {

struct RecEntry {
    std::string name;
    std::string full_path;
    uint32_t bytes = 0;
    uint32_t est_sec = 0;
};

struct SdRecGuard {
    bool spi_started = false;
    bool mounted = false;

    bool mount() {
        SPI.begin(cfg::SD_SCK, cfg::SD_MISO, cfg::SD_MOSI, cfg::SD_CS);
        spi_started = true;
        mounted = SD.begin(cfg::SD_CS, SPI, 25000000);
        return mounted;
    }

    ~SdRecGuard() {
        if (mounted) SD.end();
        if (spi_started) SPI.end();
    }
};

constexpr uint32_t kRecSampleRate = 16000;
constexpr size_t kRecChunkSamples = 512;  // 32 ms per chunk at 16 kHz mono

constexpr size_t kPlayBufCount = 2;
constexpr size_t kPlayBufSize = 2048;
uint8_t g_play_buf[kPlayBufCount][kPlayBufSize];
std::atomic<bool> g_play_busy[kPlayBufCount] = {{false}, {false}};

void wavPlayReleased(void*, const void* data, uint8_t) {
    for (size_t i = 0; i < kPlayBufCount; ++i) {
        if (data == g_play_buf[i]) {
            g_play_busy[i].store(false, std::memory_order_release);
        }
    }
}

void ensureRecordingsDir() {
    if (!SD.exists(cfg::SD_DIR_RECORDINGS)) {
        SD.mkdir(cfg::SD_DIR_RECORDINGS);
    }
}

std::vector<RecEntry> scanRecordings() {
    std::vector<RecEntry> list;
    File dir = SD.open(cfg::SD_DIR_RECORDINGS);
    if (!dir || !dir.isDirectory()) return list;

    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            std::string raw_name = f.name();
            size_t slash = raw_name.find_last_of('/');
            std::string base = (slash == std::string::npos) ? raw_name : raw_name.substr(slash + 1);
            if (audio_utils::isWavFilename(base)) {
                uint32_t sz = static_cast<uint32_t>(f.size());
                uint32_t payload = (sz > sizeof(audio_utils::WavHeader))
                                       ? (sz - static_cast<uint32_t>(sizeof(audio_utils::WavHeader)))
                                       : 0;
                uint32_t sec = payload / (kRecSampleRate * sizeof(int16_t));
                std::string path = std::string(cfg::SD_DIR_RECORDINGS) + "/" + base;
                list.push_back(RecEntry{base, path, sz, sec});
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    std::sort(list.begin(), list.end(), [](const RecEntry& a, const RecEntry& b) {
        return a.name > b.name;
    });
    return list;
}

void drawRecScreen(const std::string& filename, uint32_t elapsed_ms, int peak_lvl,
                   uint32_t bytes_written, bool warming) {
    auto& cv = ui::canvas();
    cv.fillScreen(cfg::COL_BG);
    ui::drawHeader("Voice Recorder");

    bool dot_on = ((elapsed_ms / 400) % 2) == 0;
    if (warming) {
        cv.setTextColor(cfg::COL_YELLOW, cfg::COL_BG);
        cv.drawString("Warming ES8311 mic...", 12, 34);
    } else {
        if (dot_on) cv.fillCircle(16, 36, 4, cfg::COL_RED);
        cv.setTextColor(cfg::COL_RED, cfg::COL_BG);
        cv.drawString("REC", 26, 32);
        cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        std::string tm = audio_utils::formatMmSs(elapsed_ms / 1000);
        cv.drawString(tm.c_str(), 64, 32);
        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString((std::to_string(bytes_written / 1024) + " KB").c_str(), 120, 32);
    }

    cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
    cv.drawString(ui::fitCols(filename, 36).c_str(), 10, 52);

    // VU meter bar
    cv.drawRect(10, 72, 220, 14, cfg::COL_GRAY);
    int bar_w = std::min(216, (peak_lvl * 216) / 100);
    if (bar_w > 0) {
        uint16_t bcol = (peak_lvl > 80) ? cfg::COL_RED : ((peak_lvl > 45) ? cfg::COL_YELLOW : cfg::COL_GREEN);
        cv.fillRect(12, 74, bar_w, 10, bcol);
    }

    ui::drawHints("Enter / Space stop", "Q / G0 stop");
    cv.pushSprite(0, 0);
}

// ES8311 codec on the Cardputer-Adv internal I2C bus.
constexpr uint8_t kEs8311Addr = 0x18;
constexpr uint32_t kEs8311I2cFreq = 100000;
constexpr int kWarmupChunks = 28;          // ~900 ms of discarded audio
constexpr uint32_t kRecUiIntervalMs = 120;  // VU/timer redraw throttle

bool es8311Write(uint8_t reg, uint8_t val) {
    return M5Cardputer.In_I2C.writeRegister8(kEs8311Addr, reg, val, kEs8311I2cFreq);
}

bool isStopKey(const KeyInput& k) {
    char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
    return k.enter || k.esc || k.btn_g0 || k.ch == ' ' || lc == 'q';
}

int chunkPeakPct(const int16_t* buf, size_t n) {
    int32_t max_amp = 0;
    for (size_t i = 0; i < n; ++i) {
        int32_t v = buf[i];
        if (v < 0) v = -v;
        if (v > max_amp) max_amp = v;
    }
    return std::min(100, static_cast<int>((max_amp * 100) / 16384));
}

void recordNewWav() {
    ensureRecordingsDir();

    static int16_t rec_buf[2][kRecChunkSamples];

    // Shut the speaker down cleanly: it shares the ES8311 / I2S bus with the mic.
    M5Cardputer.Speaker.setVolume(0);
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();
    delay(20);

    // M5Unified drives GPIO46 (ES8311 ADC data -> ESP32) as output-HIGH during
    // board detection and never restores it on Cardputer-Adv; the IDF 5.5 I2S
    // driver does not disable the output driver, so RX would read all-ones.
    // Reset the I2S data/clock pins so the I2S driver gets clean pins.
    gpio_reset_pin(GPIO_NUM_46);
    gpio_reset_pin(GPIO_NUM_41);
    gpio_reset_pin(GPIO_NUM_43);

    auto mcfg = M5Cardputer.Mic.config();
    mcfg.sample_rate = kRecSampleRate;
    mcfg.magnification = 4;  // codec provides analog/digital gain; keep software gain low
    M5Cardputer.Mic.config(mcfg);

    if (!M5Cardputer.Mic.begin()) {
        ui::showToast("Mic Error", "Failed to start ES8311 mic", "", cfg::COL_RED);
        return;
    }

    // ES8311 only arms its capture path when SYSTEM reg 0x0D=0x01 is written
    // while the I2S clock is running. Start a dummy capture so the clock runs,
    // then re-write 0x0D a few times (Cardputer-Adv has no post-start callback).
    M5Cardputer.Mic.record(rec_buf[0], kRecChunkSamples, kRecSampleRate);
    for (int i = 0; i < 3; ++i) {
        delay(30);
        es8311Write(0x0D, 0x01);  // SYSTEM: power up analog circuitry
    }
    // M5Unified's defaults are 0 dB PGA / 0 dB ADC, far too quiet for speech.
    es8311Write(0x0E, 0x02);  // SYSTEM: enable analog PGA and ADC modulator
    es8311Write(0x14, 0x17);  // ADC_REG14: Mic1p/Mic1n, PGA +21 dB
    es8311Write(0x17, 0xCB);  // ADC_REG17: ADC volume +6 dB
    es8311Write(0x1C, 0x6A);  // ADC_REG1C: EQ bypass, digital DC offset cancel
    while (M5Cardputer.Mic.isRecording()) {
        delay(2);
    }

    time_t now = time(nullptr);
    std::string filename;
    std::string path;
    for (int attempt = 0; attempt < 10; ++attempt) {
        filename = time_sync::formatLogFilenameStem(now + attempt) + ".wav";
        path = std::string(cfg::SD_DIR_RECORDINGS) + "/" + filename;
        if (!SD.exists(path.c_str())) break;
    }

    File file = SD.open(path.c_str(), FILE_WRITE);
    if (!file) {
        M5Cardputer.Mic.end();
        gpio_reset_pin(GPIO_NUM_41);
        gpio_reset_pin(GPIO_NUM_43);
        ui::showToast("SD Error", "Cannot open file:", filename, cfg::COL_RED);
        return;
    }

    audio_utils::WavHeader hdr = audio_utils::makePcmWavHeader(kRecSampleRate, 1, 16, 0);
    file.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));

    ui::waitKeyRelease();

    // ES8311 analog section needs ~1 s after power-up; discard initial frames.
    drawRecScreen(filename, 0, 0, 0, true);
    for (int w = 0; w < kWarmupChunks; ++w) {
        M5Cardputer.Mic.record(rec_buf[0], kRecChunkSamples, kRecSampleRate);
        bool cancel = false;
        while (M5Cardputer.Mic.isRecording()) {
            if (isStopKey(ui::pollInput(400))) cancel = true;
            delay(2);
        }
        if (cancel || isStopKey(ui::pollInput(400))) {
            file.close();
            SD.remove(path.c_str());
            M5Cardputer.Mic.end();
            gpio_reset_pin(GPIO_NUM_41);
            gpio_reset_pin(GPIO_NUM_43);
            ui::waitKeyRelease();
            return;
        }
    }

    // Double-buffered capture. Mic_Class completes queued requests in FIFO
    // order, so rec_buf[idx] is always the oldest in-flight buffer. 'inflight'
    // counts buffers queued but not yet written (oldest = idx, next = idx^1).
    uint32_t data_bytes = 0;
    uint32_t t_start = millis();
    uint32_t last_ui = 0;
    int peak_pct = 0;
    int idx = 0;
    int inflight = 0;
    bool stop = false;
    bool write_failed = false;

    if (M5Cardputer.Mic.record(rec_buf[0], kRecChunkSamples, kRecSampleRate)) {
        ++inflight;
        if (M5Cardputer.Mic.record(rec_buf[1], kRecChunkSamples, kRecSampleRate)) ++inflight;
    }
    if (inflight < 2) stop = true;  // could not queue; drain whatever started

    drawRecScreen(filename, 0, 0, 0, false);
    last_ui = millis();

    while (!stop) {
        // Wait until the oldest buffer completes (busy slot count drops below
        // the number of buffers we have in flight). Keep polling keys.
        while (static_cast<int>(M5Cardputer.Mic.isRecording()) >= inflight) {
            if (isStopKey(ui::pollInput(400))) {
                stop = true;
                break;
            }
            delay(2);
        }
        if (stop) break;

        // rec_buf[idx] is complete.
        peak_pct = chunkPeakPct(rec_buf[idx], kRecChunkSamples);
        size_t wr = file.write(reinterpret_cast<const uint8_t*>(rec_buf[idx]),
                               kRecChunkSamples * sizeof(int16_t));
        --inflight;
        if (wr == 0) {
            write_failed = true;
            stop = true;
            break;
        }
        data_bytes += static_cast<uint32_t>(wr);

        // Re-queue the just-written buffer behind the other in-flight one.
        if (M5Cardputer.Mic.record(rec_buf[idx], kRecChunkSamples, kRecSampleRate)) {
            ++inflight;
        } else {
            stop = true;
        }
        idx ^= 1;

        uint32_t now_ms = millis();
        if (now_ms - last_ui >= kRecUiIntervalMs) {
            last_ui = now_ms;
            drawRecScreen(filename, now_ms - t_start, peak_pct, data_bytes, false);
        }
    }

    // Drain: let every queued request finish, then flush the remaining
    // completed buffers in FIFO order (oldest first).
    while (M5Cardputer.Mic.isRecording()) {
        delay(2);
    }
    for (; inflight > 0 && !write_failed; --inflight) {
        size_t wr = file.write(reinterpret_cast<const uint8_t*>(rec_buf[idx]),
                               kRecChunkSamples * sizeof(int16_t));
        if (wr == 0) {
            write_failed = true;
            break;
        }
        data_bytes += static_cast<uint32_t>(wr);
        idx ^= 1;
    }

    M5Cardputer.Mic.end();
    gpio_reset_pin(GPIO_NUM_41);
    gpio_reset_pin(GPIO_NUM_43);

    hdr = audio_utils::makePcmWavHeader(kRecSampleRate, 1, 16, data_bytes);
    file.seek(0);
    file.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
    file.flush();
    file.close();

    uint32_t dur_sec = data_bytes / (kRecSampleRate * sizeof(int16_t));
    ui::showToast("Saved Recording", filename,
                  audio_utils::formatMmSs(dur_sec) + " (" + std::to_string(data_bytes / 1024) + " KB)",
                  cfg::COL_GREEN);
    ui::waitKeyRelease();
}

struct __attribute__((packed)) RawWavFmt {
    uint16_t audio_fmt;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_per_sec;
    uint16_t block_size;
    uint16_t bits_per_sample;
};

struct __attribute__((packed)) RawChunkHdr {
    char id[4];
    uint32_t size;
};

void playRecordingWav(const RecEntry& rec) {
    File f = SD.open(rec.full_path.c_str(), FILE_READ);
    if (!f) {
        ui::showToast("Open Failed", rec.name, "", cfg::COL_RED);
        return;
    }

    char riff_hdr[12];
    if (f.read(reinterpret_cast<uint8_t*>(riff_hdr), sizeof(riff_hdr)) != sizeof(riff_hdr) ||
        std::memcmp(riff_hdr, "RIFF", 4) != 0 ||
        std::memcmp(riff_hdr + 8, "WAVE", 4) != 0) {
        f.close();
        ui::showToast("Invalid WAV", rec.name, "", cfg::COL_RED);
        return;
    }

    RawWavFmt fmt{};
    bool got_fmt = false;
    uint32_t data_size = 0;
    bool got_data = false;

    RawChunkHdr chk{};
    while (f.read(reinterpret_cast<uint8_t*>(&chk), sizeof(chk)) == sizeof(chk)) {
        if (std::memcmp(chk.id, "fmt ", 4) == 0 && chk.size >= sizeof(RawWavFmt)) {
            if (f.read(reinterpret_cast<uint8_t*>(&fmt), sizeof(fmt)) != sizeof(fmt)) break;
            if (chk.size > sizeof(fmt)) {
                f.seek(chk.size - sizeof(fmt), SeekCur);
            }
            got_fmt = true;
        } else if (std::memcmp(chk.id, "data", 4) == 0) {
            data_size = chk.size;
            got_data = true;
            break;
        } else {
            if (!f.seek(chk.size, SeekCur)) break;
        }
    }

    if (!got_fmt || !got_data || fmt.audio_fmt != 1 || fmt.channels == 0 || fmt.channels > 2 ||
        (fmt.bits_per_sample != 8 && fmt.bits_per_sample != 16)) {
        f.close();
        ui::showToast("Unsupported WAV", rec.name, "Need 8/16-bit PCM WAV", cfg::COL_RED);
        return;
    }

    M5Cardputer.Mic.end();
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();
    // Return the shared I2S clock pins to a clean state before the speaker re-inits them.
    gpio_reset_pin(GPIO_NUM_41);
    gpio_reset_pin(GPIO_NUM_43);

    for (size_t i = 0; i < kPlayBufCount; ++i) {
        g_play_busy[i].store(false, std::memory_order_relaxed);
    }
    M5Cardputer.Speaker.setBufferReleaseCallback(nullptr, wavPlayReleased);

    uint8_t vol = 180;
    if (!M5Cardputer.Speaker.begin()) {
        M5Cardputer.Speaker.setBufferReleaseCallback(nullptr, nullptr);
        f.close();
        ui::showToast("Speaker Error", "Failed to start ES8311", "", cfg::COL_RED);
        return;
    }
    M5Cardputer.Speaker.setVolume(vol);

    uint32_t byte_rate = fmt.sample_rate * fmt.channels * (fmt.bits_per_sample / 8);
    uint32_t total_sec = byte_rate ? (data_size / byte_rate) : 0;
    uint32_t remaining = data_size;
    uint32_t played_bytes = 0;
    bool is_16bit = (fmt.bits_per_sample == 16);
    bool stereo = (fmt.channels > 1);
    bool paused = false;
    bool stopped = false;
    size_t idx = 0;
    uint32_t last_ui = 0;

    ui::waitKeyRelease();

    auto drawPlayUi = [&](bool force) {
        uint32_t now = millis();
        if (!force && (now - last_ui < 100)) return;
        last_ui = now;
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Voice Playback");

        cv.setTextColor(paused ? cfg::COL_YELLOW : cfg::COL_GREEN, cfg::COL_BG);
        cv.drawString(paused ? "[ PAUSED ]" : "[ PLAYING ]", 10, 26);

        int vol_pct = (static_cast<int>(vol) * 100) / 255;
        cv.setTextDatum(top_right);
        cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        cv.drawString(("Vol:" + std::to_string(vol_pct) + "%").c_str(), 230, 26);
        cv.setTextDatum(top_left);

        cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
        cv.drawString(ui::fitCols(rec.name, 36).c_str(), 10, 44);

        cv.drawRect(10, 64, 220, 10, cfg::COL_GRAY);
        int pw = data_size ? static_cast<int>((static_cast<uint64_t>(played_bytes) * 216ULL) / data_size) : 0;
        if (pw > 0) cv.fillRect(12, 66, std::min(216, pw), 6, cfg::COL_ORANGE);

        uint32_t cur_sec = byte_rate ? (played_bytes / byte_rate) : 0;
        cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
        cv.drawString((audio_utils::formatMmSs(cur_sec) + " / " + audio_utils::formatMmSs(total_sec)).c_str(), 10, 82);

        ui::drawHints("Space pause  ;. vol", "Enter / Q stop");
        cv.pushSprite(0, 0);
    };

    drawPlayUi(true);

    while (remaining > 0 && !stopped) {
        KeyInput k = ui::pollInput(120);
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
        if (k.esc || k.btn_g0 || k.enter || lc == 'q') {
            stopped = true;
            break;
        }
        if (k.ch == ' ') {
            paused = !paused;
            drawPlayUi(true);
        } else if (k.up || k.ch == '+' || k.ch == '=') {
            vol = static_cast<uint8_t>(std::min(255, static_cast<int>(vol) + 16));
            M5Cardputer.Speaker.setVolume(vol);
            drawPlayUi(true);
        } else if (k.down || k.ch == '-') {
            vol = static_cast<uint8_t>(std::max(0, static_cast<int>(vol) - 16));
            M5Cardputer.Speaker.setVolume(vol);
            drawPlayUi(true);
        }

        if (paused) {
            delay(20);
            continue;
        }

        if (g_play_busy[idx].load(std::memory_order_acquire)) {
            drawPlayUi(false);
            delay(2);
            continue;
        }

        size_t req = std::min<size_t>(kPlayBufSize, remaining);
        size_t n = f.read(g_play_buf[idx], req);
        if (is_16bit) n &= ~static_cast<size_t>(1);
        if (n == 0) break;

        remaining -= static_cast<uint32_t>(n);
        played_bytes += static_cast<uint32_t>(n);

        g_play_busy[idx].store(true, std::memory_order_release);
        bool ok = is_16bit
                      ? M5Cardputer.Speaker.playRaw(reinterpret_cast<const int16_t*>(g_play_buf[idx]),
                                                    n >> 1, fmt.sample_rate, stereo, 1, 0)
                      : M5Cardputer.Speaker.playRaw(g_play_buf[idx], n, fmt.sample_rate, stereo, 1, 0);
        if (!ok) {
            g_play_busy[idx].store(false, std::memory_order_release);
        }
        idx = (idx + 1) % kPlayBufCount;
        drawPlayUi(false);
    }

    if (!stopped) {
        drawPlayUi(true);
        uint32_t drain_t0 = millis();
        while ((g_play_busy[0].load(std::memory_order_acquire) ||
                g_play_busy[1].load(std::memory_order_acquire)) &&
               (millis() - drain_t0 < 2000)) {
            KeyInput k = ui::pollInput();
            char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
            if (k.esc || k.btn_g0 || k.enter || lc == 'q') break;
            delay(10);
        }
    }

    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();
    M5Cardputer.Speaker.setBufferReleaseCallback(nullptr, nullptr);
    f.close();
    ui::waitKeyRelease();
}

}  // namespace

void runRecorderApp() {
    ui::waitKeyRelease();
    SdRecGuard sd;
    if (!sd.mount()) {
        ui::showToast("SD Error", "SD card not detected", "Insert FAT32 microSD card", cfg::COL_RED);
        return;
    }
    ensureRecordingsDir();

    auto list = scanRecordings();
    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 5;

    while (true) {
        int total_rows = 1 + static_cast<int>(list.size());
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Voice Recorder");

        for (int i = 0; i < kMaxVis && (scroll + i) < total_rows; ++i) {
            int idx = scroll + i;
            int y = 26 + i * 18;
            bool sel = (idx == cursor);
            if (sel) {
                cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(idx == 0 ? cfg::COL_GREEN : cfg::COL_CREAM, cfg::COL_BG);
            }

            if (idx == 0) {
                cv.drawString("[ + New Recording ]", 10, y + 2);
            } else {
                const auto& rec = list[idx - 1];
                cv.drawString(ui::fitCols(rec.name, 26).c_str(), 8, y + 2);
                if (!sel) cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
                cv.setTextDatum(top_right);
                cv.drawString(audio_utils::formatMmSs(rec.est_sec).c_str(), 232, y + 2);
                cv.setTextDatum(top_left);
            }
        }

        ui::drawHints(cursor == 0 ? "Enter start rec" : "Enter play  D del", "Q / G0 back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        if (k.up) {
            cursor = (cursor - 1 + total_rows) % total_rows;
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, total_rows - kMaxVis);
        } else if (k.down) {
            cursor = (cursor + 1) % total_rows;
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (lc == 'd' && cursor > 0) {
            const auto& rec = list[cursor - 1];
            if (ui::promptConfirm("Delete Recording?", rec.name, "/recordings/")) {
                SD.remove(rec.full_path.c_str());
                list = scanRecordings();
                int new_total = 1 + static_cast<int>(list.size());
                if (cursor >= new_total) cursor = new_total - 1;
                if (cursor < scroll) scroll = cursor;
            }
            ui::waitKeyRelease();
        } else if (k.enter) {
            if (cursor == 0) {
                recordNewWav();
                list = scanRecordings();
                cursor = list.empty() ? 0 : 1;
                scroll = 0;
            } else {
                playRecordingWav(list[cursor - 1]);
            }
        }
        delay(25);
    }
}
