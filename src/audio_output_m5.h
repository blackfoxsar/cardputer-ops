#pragma once

#include <AudioOutput.h>
#include <M5Unified.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

class AudioOutputM5Speaker : public AudioOutput {
public:
    AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t virtual_sound_channel = 0)
        : m5sound_(m5sound), virtual_ch_(virtual_sound_channel) {}

    ~AudioOutputM5Speaker() override = default;

    void setup() {
        aborted_.store(false, std::memory_order_release);
        for (size_t i = 0; i < kBufCount; ++i) {
            busy_[i].store(false, std::memory_order_relaxed);
        }
        buffer_index_ = 0;
        index_ = 0;
        frames_.store(0, std::memory_order_relaxed);
        m5sound_->setBufferReleaseCallback(this, bufferReleased);
    }

    void teardown() {
        aborted_.store(true, std::memory_order_release);
        m5sound_->stop();
        m5sound_->end();
        m5sound_->setBufferReleaseCallback(nullptr, nullptr);
    }

    void abortNow() {
        aborted_.store(true, std::memory_order_release);
        m5sound_->stop();
    }

    void clearAbort() {
        aborted_.store(false, std::memory_order_release);
    }

    void resetFrames() {
        frames_.store(0, std::memory_order_relaxed);
    }

    bool begin() override { return true; }

    bool ConsumeSample(int16_t sample[2]) override {
        if (aborted_.load(std::memory_order_acquire)) {
            return false;
        }
        if (buffer_index_ < kBufSize) {
            buffer_[index_][buffer_index_] = sample[0];
            buffer_[index_][buffer_index_ + 1] = sample[1];
            buffer_index_ += 2;
            return true;
        }
        flush();
        return false;
    }

    void flush() override {
        if (buffer_index_ == 0) return;
        if (aborted_.load(std::memory_order_acquire)) {
            buffer_index_ = 0;
            return;
        }
        busy_[index_].store(true, std::memory_order_release);
        bool queued = m5sound_->playRaw(buffer_[index_], buffer_index_, hertz, true, 1, virtual_ch_);
        if (queued) {
            frames_.fetch_add(static_cast<uint32_t>(buffer_index_ / 2), std::memory_order_relaxed);
        } else {
            busy_[index_].store(false, std::memory_order_release);
        }
        index_ = (index_ + 1) % kBufCount;
        buffer_index_ = 0;
        while (busy_[index_].load(std::memory_order_acquire)) {
            if (aborted_.load(std::memory_order_acquire)) break;
            vTaskDelay(1);
        }
    }

    bool stop() override {
        if (!aborted_.load(std::memory_order_acquire)) {
            flush();
            uint32_t t0 = millis();
            for (size_t i = 0; i < kBufCount; ++i) {
                while (busy_[i].load(std::memory_order_acquire) && (millis() - t0 < 1500)) {
                    if (aborted_.load(std::memory_order_acquire)) break;
                    vTaskDelay(1);
                }
            }
        }
        buffer_index_ = 0;
        return true;
    }

    uint32_t getFrames() const { return frames_.load(std::memory_order_relaxed); }
    uint32_t getRate() const { return hertz; }

private:
    static void bufferReleased(void* args, const void* data, uint8_t) {
        auto* me = static_cast<AudioOutputM5Speaker*>(args);
        for (size_t i = 0; i < kBufCount; ++i) {
            if (data == me->buffer_[i]) {
                me->busy_[i].store(false, std::memory_order_release);
            }
        }
    }

    static constexpr size_t kBufCount = 3;
    static constexpr size_t kBufSize = 1024;  // 512 stereo frames

    m5::Speaker_Class* m5sound_;
    uint8_t virtual_ch_;
    int16_t buffer_[kBufCount][kBufSize]{};
    std::atomic<bool> busy_[kBufCount] = {{false}, {false}, {false}};
    std::atomic<bool> aborted_{false};
    std::atomic<uint32_t> frames_{0};
    size_t buffer_index_ = 0;
    size_t index_ = 0;
};
