#include "audio_hw.h"

#include <M5Cardputer.h>
#include <driver/gpio.h>

#include "audio_utils.h"

namespace {

constexpr uint8_t kEs8311Addr = 0x18;

void es8311Write(uint8_t reg, uint8_t val) {
    M5Cardputer.In_I2C.writeRegister8(kEs8311Addr, reg, val, 100000);
}

void resetSharedAudioBus() {
    M5Cardputer.Mic.end();
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();
    gpio_reset_pin(GPIO_NUM_41);
    gpio_reset_pin(GPIO_NUM_43);
}

}  // namespace

namespace audio_hw {

void restoreQuietSpeakerIdle() {
    resetSharedAudioBus();
    delay(15);

    // Briefly initialize the speaker path with zeroed samples so ES8311 brings
    // VMID, DAC, and HP drive up with zero DC offset instead of leaving outputs floating.
    if (M5Cardputer.Speaker.begin()) {
        static int16_t kZeroSamples[64]{};
        M5Cardputer.Speaker.setVolume(0);
        M5Cardputer.Speaker.playRaw(kZeroSamples, 64, 16000, false, 1, 0, true);
        delay(15);
        M5Cardputer.Speaker.stop();
        M5Cardputer.Speaker.end();
    }

    // Power down mic PGA/ADC and mute DAC digitally while keeping HP output cleanly biased.
    es8311Write(0x0E, 0x6A);
    es8311Write(0x32, 0x00);
}

bool prepareSpeaker(int vol_pct) {
    resetSharedAudioBus();
    delay(10);
    if (!M5Cardputer.Speaker.begin()) {
        return false;
    }
    M5Cardputer.Speaker.setVolume(audio_utils::volumePctToHardware(vol_pct));
    return true;
}

bool prepareMic(uint8_t adc_pga_reg14, uint8_t adc_vol_reg17) {
    M5Cardputer.Mic.end();
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();
    delay(20);

    // M5Unified leaves GPIO46 output-HIGH after board detection; reset all three
    // ES8311 I2S pins before acquiring the microphone driver.
    gpio_reset_pin(GPIO_NUM_46);
    gpio_reset_pin(GPIO_NUM_41);
    gpio_reset_pin(GPIO_NUM_43);

    auto mcfg = M5Cardputer.Mic.config();
    mcfg.sample_rate = 16000;
    mcfg.magnification = 4;
    M5Cardputer.Mic.config(mcfg);

    if (!M5Cardputer.Mic.begin()) {
        return false;
    }

    // Mute speaker DAC output while mic is capturing.
    es8311Write(0x32, 0x00);

    static int16_t dummy_buf[256];
    M5Cardputer.Mic.record(dummy_buf, 256, 16000);
    for (int i = 0; i < 3; ++i) {
        delay(30);
        es8311Write(0x0D, 0x01);  // Power up analog circuitry while I2S clock runs
    }
    es8311Write(0x0E, 0x02);           // Enable analog PGA and ADC modulator
    es8311Write(0x14, adc_pga_reg14);  // Mic1p/Mic1n + PGA gain
    es8311Write(0x17, adc_vol_reg17);  // ADC digital volume
    es8311Write(0x1C, 0x6A);           // Bypass ADC EQ, cancel digital DC offset
    while (M5Cardputer.Mic.isRecording()) {
        delay(2);
    }
    delay(40);
    return true;
}

}  // namespace audio_hw
