#pragma once

#include "base_effect_module.h"
#include "../Util/playing_head.h"

namespace bkshepherd
{

static constexpr size_t kMicroLoopMaxSize = 48000 * 5;  // 5 seconds at 48kHz

class MicroLooperModule : public BaseEffectModule
{
  public:
    MicroLooperModule();
    ~MicroLooperModule() override;

    enum Param {
        PARAM_COUNT
    };

    void Init(float sample_rate) override;
    void ProcessStereo(float inL, float inR) override;
    bool Poll() override;
    void AlternateFootswitchPressed() override;
    float GetBrightnessForLED(int led_id) const override;

  private:
    void ResetBuffer();

    // Loop buffer - stored in SDRAM
    static float DSY_SDRAM_BSS buffer_[kMicroLoopMaxSize];

    // Recording state
    bool midi_sync_ = false;
    bool armed_recording_ = false;
    bool armed_stop_ = false;
    bool clock_beat_ = false;
    bool is_recording_ = false;
    bool is_playing_ = false;
    bool first_layer_ = true;
    size_t loop_length_ = 0;
    size_t mod_ = kMicroLoopMaxSize;
    size_t write_pos_ = 0;
    size_t read_pos_ = 0;

    PlayingHead playing_head_;
    PlayingHead recording_head_;

    void WriteBuffer(float in);
};

} // namespace bkshepherd
