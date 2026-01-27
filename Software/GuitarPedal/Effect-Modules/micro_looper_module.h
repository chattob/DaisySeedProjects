#pragma once

#include "base_effect_module.h"
#include "../Util/playing_head.h"

namespace bkshepherd
{

// ============================================================
// FFT CONFIGURATION
// ============================================================
static constexpr size_t N = 8192;          // FFT size
static constexpr size_t H_IN = N / 2;      // Input hop (analysis)
static constexpr size_t STRETCH = 40;       // Stretch factor
static constexpr size_t H_OUT = N / 4;     // Output hop (synthesis)
static constexpr size_t OUT_RING = 4 * N;
static constexpr size_t kStretchClearChunk = 2048;

static constexpr size_t kMicroLoopMaxSize = 16384 * 2;
// Ensure stretched buffer size is a multiple of H_OUT for proper circular OLA
static constexpr size_t kMicroLoopMaxStretchedSize = ((STRETCH * kMicroLoopMaxSize) / H_OUT) * H_OUT;

class MicroLooperModule : public BaseEffectModule
{
  public:
    MicroLooperModule();
    ~MicroLooperModule() override;

    enum Param {
      LOOP_MODE,
      SPEED,
      LOOP_MIX,
      PARAM_COUNT
    };

    enum LoopMode {
      OVERDUB,
      SAMPLER
    };

    void Init(float sample_rate) override;
    void ProcessStereo(float inL, float inR) override;
    bool Poll() override;
    void BypassFootswitchPressed() override;
    float GetBrightnessForLED(int led_id) const override;

  private:
    void ResetBuffer();

    // Loop buffer - stored in SDRAM
    static float DSY_SDRAM_BSS buffer_[kMicroLoopMaxSize];
    static float DSY_SDRAM_BSS stretched_buffer_[kMicroLoopMaxStretchedSize];

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
    size_t prev_wraparound_count_ = 0;

    void WriteBuffer(float in);
    void StartStretching();

    // Stretching state
    bool is_stretching_ = false;
    bool streaming_stretch_ = false;   // True when stretching while still recording
    bool use_stretched_buffer_ = false;
    bool stretched_buffer_normalized_ = false;
    size_t stretch_read_pos_ = 0;      // Position in source buffer_
    size_t stretch_write_pos_ = 0;     // Position in stretched_buffer_
    size_t stretched_length_ = 0;      // Final length of stretched buffer
    size_t stretched_ready_length_ = 0;
    size_t stretch_total_frames_ = 0;
    size_t stretch_frames_done_ = 0;
    size_t stretch_output_frames_done_ = 0;
    PlayingHead stretch_playing_head_;
    bool stretch_clear_pending_ = false;
    size_t stretch_clear_pos_ = 0;
};

} // namespace bkshepherd
