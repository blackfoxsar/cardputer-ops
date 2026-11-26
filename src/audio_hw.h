#pragma once

#include <stdint.h>

namespace audio_hw {

// Switch ES8311 cleanly from Mic or Speaker back to a silent speaker idle state
// with zero DC bias so the speaker amplifier does not emit static hiss.
void restoreQuietSpeakerIdle();

// Reset I2S bus and start ES8311 speaker at the given 0..100 volume percentage.
bool prepareSpeaker(int vol_pct);

// Reset I2S bus, release GPIO46, start ES8311 microphone at 16 kHz mono,
// and configure analog PGA + ADC gain while the I2S clock runs.
bool prepareMic(uint8_t adc_pga_reg14 = 0x17, uint8_t adc_vol_reg17 = 0xCB);

}  // namespace audio_hw
