#include "app_music.h"

#include <AudioFileSourceFS.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "audio_output_m5.h"
#include "audio_utils.h"
#include "config.h"
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

    ~SdMusicGuard() {
        if (mounted) SD.end();
        if (spi_started) SPI.end();
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

struct PlayerCtx {
    const std::vector<MusicTrack>* tracks = nullptr;
    AudioOutputM5Speaker* out = nullptr;
    SemaphoreHandle_t mtx = nullptr;
    SemaphoreHandle_t done = nullptr;

    std::atomic<int> cur_idx{0};
    std::atomic<int> track_req{0};
    std::atomic<bool> paused{false};
    std::atomic<bool> quit{false};
    std::atomic<bool> active{false};
    std::atomic<int> repeat_mode{0};

    std::string meta_file;
    std::string meta_title;
    std::string meta_artist;
    std::string meta_err;
};

void id3MetaCallback(void* cbData, const char* type, bool, const char* str) {
    auto* ctx = static_cast<PlayerCtx*>(cbData);
    if (!ctx || !str || !str[0] || !type) return;
    if (std::strcmp(type, "Title") == 0) {
        xSemaphoreTake(ctx->mtx, portMAX_DELAY);
        ctx->meta_title = str;
        xSemaphoreGive(ctx->mtx);
    } else if (std::strcmp(type, "Performer") == 0 || std::strcmp(type, "Artist") == 0) {
        xSemaphoreTake(ctx->mtx, portMAX_DELAY);
        ctx->meta_artist = str;
        xSemaphoreGive(ctx->mtx);
    }
}

bool endsWithWav(const std::string& path) {
    return audio_utils::isWavFilename(path);
}

void audioDecodeTask(void* arg) {
    auto* ctx = static_cast<PlayerCtx*>(arg);
    AudioFileSourceFS fs_src(SD);
    AudioGeneratorMP3* mp3 = new AudioGeneratorMP3();
    AudioGeneratorWAV* wav = new AudioGeneratorWAV();
    AudioGenerator* gen = nullptr;
    AudioFileSourceID3* id3 = nullptr;

    auto stopCurrent = [&]() {
        if (gen) {
            ctx->out->stop();
            gen->stop();
            gen = nullptr;
        }
        if (id3) {
            id3->RegisterMetadataCB(nullptr, nullptr);
            id3->close();
            delete id3;
            id3 = nullptr;
        }
        fs_src.close();
        ctx->active.store(false, std::memory_order_release);
    };

    auto startTrack = [&](int index) -> bool {
        stopCurrent();
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

        ctx->out->clearAbort();
        ctx->out->resetFrames();

        if (!fs_src.open(tr.path.c_str())) {
            xSemaphoreTake(ctx->mtx, portMAX_DELAY);
            ctx->meta_err = "Cannot open file";
            xSemaphoreGive(ctx->mtx);
            return false;
        }

        bool ok = false;
        if (endsWithWav(tr.name)) {
            gen = wav;
            ok = gen->begin(&fs_src, ctx->out);
        } else {
            id3 = new AudioFileSourceID3(&fs_src);
            id3->RegisterMetadataCB(id3MetaCallback, ctx);
            id3->open(tr.path.c_str());
            gen = mp3;
            ok = gen->begin(id3, ctx->out);
        }

        if (!ok) {
            stopCurrent();
            xSemaphoreTake(ctx->mtx, portMAX_DELAY);
            ctx->meta_err = "Decode error";
            xSemaphoreGive(ctx->mtx);
            return false;
        }
        ctx->active.store(true, std::memory_order_release);
        return true;
    };

    auto advanceTrack = [&](int step) {
        int n = static_cast<int>(ctx->tracks->size());
        int base = ctx->cur_idx.load(std::memory_order_acquire);
        for (int tries = 0; tries < n && !ctx->quit.load(std::memory_order_acquire); ++tries) {
            int nxt = (((base + step) % n) + n) % n;
            if (startTrack(nxt)) return;
            base = nxt;
            step = (step >= 0) ? 1 : -1;
        }
    };

    startTrack(ctx->cur_idx.load(std::memory_order_acquire));

    while (!ctx->quit.load(std::memory_order_acquire)) {
        int step = ctx->track_req.exchange(0);
        if (step != 0) {
            ctx->paused.store(false, std::memory_order_release);
            advanceTrack(step);
            continue;
        }

        if (ctx->paused.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        if (gen && gen->isRunning()) {
            if (!gen->loop()) {
                int rm = ctx->repeat_mode.load(std::memory_order_acquire);
                if (rm == static_cast<int>(RepeatMode::ONE)) {
                    startTrack(ctx->cur_idx.load(std::memory_order_acquire));
                } else if (rm == static_cast<int>(RepeatMode::ALL)) {
                    advanceTrack(1);
                } else {
                    int nxt = ctx->cur_idx.load(std::memory_order_acquire) + 1;
                    if (nxt < static_cast<int>(ctx->tracks->size())) {
                        advanceTrack(1);
                    } else {
                        stopCurrent();
                        xSemaphoreTake(ctx->mtx, portMAX_DELAY);
                        ctx->meta_err = "Playback finished";
                        xSemaphoreGive(ctx->mtx);
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    ctx->out->abortNow();
    stopCurrent();
    delete mp3;
    delete wav;
    xSemaphoreGive(ctx->done);
    vTaskDelete(nullptr);
}

// Returns true to keep running list (L / track ended -> back), false to exit Music app
bool runNowPlaying(const std::vector<MusicTrack>& tracks, int& io_cursor) {
    M5Cardputer.Mic.end();
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();

    AudioOutputM5Speaker out(&M5Cardputer.Speaker, 0);
    out.setup();

    uint8_t vol = 160;
    if (!M5Cardputer.Speaker.begin()) {
        out.teardown();
        ui::showToast("Speaker Error", "Failed to start ES8311", "", cfg::COL_RED);
        return true;
    }
    M5Cardputer.Speaker.setVolume(vol);

    PlayerCtx ctx;
    ctx.tracks = &tracks;
    ctx.out = &out;
    ctx.mtx = xSemaphoreCreateMutex();
    ctx.done = xSemaphoreCreateBinary();
    ctx.cur_idx.store(io_cursor, std::memory_order_release);

    TaskHandle_t task_h = nullptr;
    BaseType_t ok = xTaskCreate(audioDecodeTask, "mp3_dec", 12288, &ctx, 2, &task_h);
    if (ok != pdPASS) {
        vSemaphoreDelete(ctx.mtx);
        vSemaphoreDelete(ctx.done);
        out.teardown();
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
            xSemaphoreTake(ctx.mtx, portMAX_DELAY);
            file_s = ctx.meta_file;
            title_s = ctx.meta_title;
            artist_s = ctx.meta_artist;
            err_s = ctx.meta_err;
            xSemaphoreGive(ctx.mtx);

            int idx = ctx.cur_idx.load(std::memory_order_acquire);
            bool paused = ctx.paused.load(std::memory_order_acquire);
            bool playing = ctx.active.load(std::memory_order_acquire);
            RepeatMode rm = static_cast<RepeatMode>(ctx.repeat_mode.load(std::memory_order_acquire));

            uint32_t rate = out.getRate();
            uint32_t sec = rate ? (out.getFrames() / rate) : 0;
            int vol_pct = (static_cast<int>(vol) * 100) / 255;

            auto& cv = ui::canvas();
            cv.fillScreen(cfg::COL_BG);
            ui::drawHeader("Music Player");

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

            ui::drawHints("Space:Pause ,/:Prev/Nxt", ";.:Vol M:Rep Q:List");
            cv.pushSprite(0, 0);
        }

        KeyInput k = ui::pollInput(160);
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0 || k.enter || lc == 'q' || lc == 'l') {
            back_to_list = (k.enter || lc == 'l' || lc == 'q' || k.esc || k.btn_g0);
            break;
        }
        if (k.ch == ' ') {
            ctx.paused.store(!ctx.paused.load(std::memory_order_acquire), std::memory_order_release);
            last_draw = 0;
        } else if (k.left) {
            out.abortNow();
            ctx.track_req.fetch_sub(1);
            last_draw = 0;
        } else if (k.right) {
            out.abortNow();
            ctx.track_req.fetch_add(1);
            last_draw = 0;
        } else if (k.up || k.ch == '+' || k.ch == '=') {
            vol = static_cast<uint8_t>(std::min(255, static_cast<int>(vol) + 16));
            M5Cardputer.Speaker.setVolume(vol);
            last_draw = 0;
        } else if (k.down || k.ch == '-') {
            vol = static_cast<uint8_t>(std::max(0, static_cast<int>(vol) - 16));
            M5Cardputer.Speaker.setVolume(vol);
            last_draw = 0;
        } else if (lc == 'm') {
            int rm = (ctx.repeat_mode.load(std::memory_order_acquire) + 1) % 3;
            ctx.repeat_mode.store(rm, std::memory_order_release);
            last_draw = 0;
        }
        delay(20);
    }

    io_cursor = ctx.cur_idx.load(std::memory_order_acquire);
    ctx.quit.store(true, std::memory_order_release);
    out.abortNow();
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(3000));
    out.teardown();
    vSemaphoreDelete(ctx.mtx);
    vSemaphoreDelete(ctx.done);
    ui::waitKeyRelease();
    return back_to_list;
}

}  // namespace

void runMusicApp() {
    ui::waitKeyRelease();
    SdMusicGuard sd;
    if (!sd.mount()) {
        ui::showToast("SD Error", "SD card not detected", "Insert FAT32 microSD card", cfg::COL_RED);
        return;
    }
    ensureMusicDir();
    auto tracks = scanMusicTracks();

    if (tracks.empty()) {
        ui::showToast("Music Player", "No audio files in /music/",
                      "Put .mp3 or .wav files in /music/", cfg::COL_YELLOW);
        return;
    }

    int cursor = 0;
    int scroll = 0;
    constexpr int kMaxVis = 5;

    while (true) {
        auto& cv = ui::canvas();
        cv.fillScreen(cfg::COL_BG);
        ui::drawHeader("Music (/music)");

        for (int i = 0; i < kMaxVis && (scroll + i) < static_cast<int>(tracks.size()); ++i) {
            int idx = scroll + i;
            int y = 26 + i * 18;
            bool sel = (idx == cursor);
            if (sel) {
                cv.fillRect(4, y - 2, 232, 16, cfg::COL_ORANGE);
                cv.setTextColor(cfg::COL_BG, cfg::COL_ORANGE);
            } else {
                cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            }
            cv.drawString(ui::fitCols(tracks[idx].name, 29).c_str(), 8, y + 2);
            if (!sel) cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            cv.setTextDatum(top_right);
            uint32_t mb10 = (tracks[idx].size * 10UL) / (1024UL * 1024UL);
            std::string sz_s = (tracks[idx].size >= 1024UL * 1024UL)
                                   ? (std::to_string(mb10 / 10) + "." + std::to_string(mb10 % 10) + "M")
                                   : (std::to_string(std::max<uint32_t>(1, tracks[idx].size / 1024)) + "K");
            cv.drawString(sz_s.c_str(), 232, y + 2);
            cv.setTextDatum(top_left);
        }

        ui::drawHints("Enter play", "R rescan  Q back");
        cv.pushSprite(0, 0);

        KeyInput k = ui::pollInput();
        ui::applyMenuNav(k);
        char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;

        if (k.esc || k.btn_g0 || lc == 'q') {
            ui::waitKeyRelease();
            return;
        }
        if (lc == 'r') {
            tracks = scanMusicTracks();
            if (tracks.empty()) {
                ui::showToast("Music Player", "No audio files in /music/", "", cfg::COL_YELLOW);
                return;
            }
            cursor = scroll = 0;
        } else if (k.up) {
            cursor = (cursor - 1 + static_cast<int>(tracks.size())) % static_cast<int>(tracks.size());
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, static_cast<int>(tracks.size()) - kMaxVis);
        } else if (k.down) {
            cursor = (cursor + 1) % static_cast<int>(tracks.size());
            if (cursor >= scroll + kMaxVis) scroll = cursor - kMaxVis + 1;
            if (cursor < scroll) scroll = 0;
        } else if (k.enter) {
            runNowPlaying(tracks, cursor);
            if (cursor < scroll) scroll = cursor;
            if (cursor >= scroll + kMaxVis) scroll = std::max(0, cursor - kMaxVis + 1);
        }
        delay(25);
    }
}
