#include "app_db_meter.h"

#include <M5Cardputer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "app_alarm.h"
#include "audio_hw.h"
#include "audio_utils.h"
#include "config.h"
#include "ui_common.h"

namespace {

constexpr size_t kChunkSamples = 512;  // 32 ms @ 16 kHz
constexpr int kWarmupChunks = 20;
constexpr int kHistoryPts = 52;

const char* levelLabel(float db) {
    if (db < 45.0f) return "Quiet";
    if (db < 65.0f) return "Normal";
    if (db < 80.0f) return "Loud";
    return "High!";
}

uint16_t levelColor(float db) {
    if (db < 45.0f) return cfg::COL_GREEN;
    if (db < 65.0f) return cfg::COL_CYAN;
    if (db < 80.0f) return cfg::COL_YELLOW;
    return cfg::COL_RED;
}

double computeAcRms(const int16_t* samples, size_t count) {
    if (count == 0) return 0.0;
    double mean = 0.0;
    for (size_t i = 0; i < count; ++i) {
        mean += static_cast<double>(samples[i]);
    }
    mean /= static_cast<double>(count);

    double sq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double d = static_cast<double>(samples[i]) - mean;
        sq += d * d;
    }
    return std::sqrt(sq / static_cast<double>(count));
}

}  // namespace

void runDbMeterApp() {
    app_alarm::SuppressGuard alarm_guard;
    ui::waitKeyRelease();

    auto& cv = ui::canvas();
    cv.fillScreen(cfg::COL_BG);
    ui::drawHeader("dB Meter");
    cv.setTextColor(cfg::COL_YELLOW, cfg::COL_BG);
    cv.drawString("Starting ES8311 mic...", 12, 48);
    ui::drawHints("Please wait", "ESC / G0 cancel");
    cv.pushSprite(0, 0);

    if (!audio_hw::prepareMic(0x17, 0xCB)) {
        audio_hw::restoreQuietSpeakerIdle();
        ui::showToast("Mic Error", "Failed to start ES8311 mic", "", cfg::COL_RED);
        return;
    }

    static int16_t buf[kChunkSamples];

    // Warm up analog mic bias before measuring.
    for (int w = 0; w < kWarmupChunks; ++w) {
        M5Cardputer.Mic.record(buf, kChunkSamples, 16000);
        while (M5Cardputer.Mic.isRecording()) {
            KeyInput k = ui::pollInput();
            if (k.esc || k.btn_g0) {
                audio_hw::restoreQuietSpeakerIdle();
                ui::waitKeyRelease();
                return;
            }
            delay(4);
        }
    }

    float cal_offset = 96.0f;
    float cur_db = 40.0f;
    float min_db = 120.0f;
    float max_db = 0.0f;
    double sum_db = 0.0;
    uint32_t sample_cnt = 0;
    bool paused = false;

    float hist[kHistoryPts]{};
    int hist_len = 0;
    int hist_head = 0;

    uint32_t last_draw_ms = 0;

    while (true) {
        if (!paused) {
            M5Cardputer.Mic.record(buf, kChunkSamples, 16000);
            while (M5Cardputer.Mic.isRecording()) {
                KeyInput k = ui::pollInput(90);
                ui::applyMenuNav(k);
                char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
                if (k.esc || k.btn_g0) {
                    audio_hw::restoreQuietSpeakerIdle();
                    ui::waitKeyRelease();
                    return;
                }
                if (k.ch == ' ') paused = !paused;
                else if (lc == 'r') {
                    min_db = 120.0f;
                    max_db = 0.0f;
                    sum_db = 0.0;
                    sample_cnt = 0;
                    hist_len = 0;
                    hist_head = 0;
                } else if (k.left || k.ch == '-') {
                    cal_offset = std::max(60.0f, cal_offset - 1.0f);
                } else if (k.right || k.ch == '+' || k.ch == '=') {
                    cal_offset = std::min(130.0f, cal_offset + 1.0f);
                }
                delay(4);
            }

            double rms = computeAcRms(buf, kChunkSamples);
            float inst_db = audio_utils::pcmRmsToDbSpl(rms, cal_offset);
            if (sample_cnt == 0) {
                cur_db = inst_db;
            } else {
                cur_db = 0.35f * inst_db + 0.65f * cur_db;
            }
            min_db = std::min(min_db, cur_db);
            max_db = std::max(max_db, cur_db);
            sum_db += cur_db;
            ++sample_cnt;

            hist[hist_head] = cur_db;
            hist_head = (hist_head + 1) % kHistoryPts;
            if (hist_len < kHistoryPts) ++hist_len;
        } else {
            KeyInput k = ui::pollInput(90);
            ui::applyMenuNav(k);
            char lc = k.ch ? static_cast<char>(std::tolower(static_cast<unsigned char>(k.ch))) : 0;
            if (k.esc || k.btn_g0) break;
            if (k.ch == ' ') paused = false;
            else if (lc == 'r') {
                min_db = 120.0f;
                max_db = 0.0f;
                sum_db = 0.0;
                sample_cnt = 0;
                hist_len = 0;
                hist_head = 0;
                last_draw_ms = 0;
            } else if (k.left || k.ch == '-') {
                cal_offset = std::max(60.0f, cal_offset - 1.0f);
                last_draw_ms = 0;
            } else if (k.right || k.ch == '+' || k.ch == '=') {
                cal_offset = std::min(130.0f, cal_offset + 1.0f);
                last_draw_ms = 0;
            }
            delay(20);
        }

        uint32_t now_ms = millis();
        if (now_ms - last_draw_ms >= 90) {
            last_draw_ms = now_ms;

            cv.fillScreen(cfg::COL_BG);
            ui::drawHeader("dB Meter");

            uint16_t col = levelColor(cur_db);
            char main_buf[16];
            std::snprintf(main_buf, sizeof(main_buf), "%.1f", cur_db);
            cv.setTextSize(3);
            cv.setTextColor(col, cfg::COL_BG);
            cv.drawString(main_buf, 10, 26);
            cv.setTextSize(1);
            cv.setTextColor(cfg::COL_CREAM, cfg::COL_BG);
            cv.drawString("dB", 88, 42);
            cv.setTextColor(col, cfg::COL_BG);
            cv.drawString(paused ? "HOLD" : levelLabel(cur_db), 88, 28);

            // Stats on right side
            float avg_db = sample_cnt ? static_cast<float>(sum_db / sample_cnt) : cur_db;
            char sbuf[24];
            cv.setTextDatum(top_right);
            cv.setTextColor(cfg::COL_GRAY, cfg::COL_BG);
            std::snprintf(sbuf, sizeof(sbuf), "Min: %.0f dB", sample_cnt ? min_db : cur_db);
            cv.drawString(sbuf, 232, 24);
            std::snprintf(sbuf, sizeof(sbuf), "Avg: %.0f dB", avg_db);
            cv.drawString(sbuf, 232, 35);
            cv.setTextColor(cfg::COL_ORANGE, cfg::COL_BG);
            std::snprintf(sbuf, sizeof(sbuf), "Max: %.0f dB", max_db);
            cv.drawString(sbuf, 232, 46);
            cv.setTextColor(cfg::COL_DIM, cfg::COL_BG);
            std::snprintf(sbuf, sizeof(sbuf), "Cal: %+.0f", cal_offset - 96.0f);
            cv.drawString(sbuf, 232, 57);
            cv.setTextDatum(top_left);

            // Live horizontal meter bar (25..110 dB)
            cv.drawRect(10, 68, 220, 10, cfg::COL_GRAY);
            float frac = std::max(0.0f, std::min(1.0f, (cur_db - 25.0f) / 85.0f));
            int bw = static_cast<int>(frac * 216.0f);
            if (bw > 0) {
                cv.fillRect(12, 70, bw, 6, col);
            }

            // Rolling history graph (y=83..113)
            cv.drawRect(10, 82, 220, 32, cfg::COL_DIM);
            int start_idx = (hist_head - hist_len + kHistoryPts) % kHistoryPts;
            for (int i = 0; i < hist_len; ++i) {
                float v = hist[(start_idx + i) % kHistoryPts];
                float hf = std::max(0.0f, std::min(1.0f, (v - 25.0f) / 85.0f));
                int bh = std::max(1, static_cast<int>(hf * 28.0f));
                int bx = 14 + i * 4;
                cv.fillRect(bx, 112 - bh, 3, bh, levelColor(v));
            }

            ui::drawHints("R rst  ,/. cal  Spc hold", "ESC / G0 back");
            cv.pushSprite(0, 0);
        }
    }

    audio_hw::restoreQuietSpeakerIdle();
    ui::waitKeyRelease();
}
