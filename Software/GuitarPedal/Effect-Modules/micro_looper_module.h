#pragma once

#include "base_effect_module.h"
#include "../Util/playing_head.h"
#include "../Util/tape_modulator.h"

namespace bkshepherd
{

// ============================================================
// FFT CONFIGURATION
// ============================================================
static constexpr size_t N = 16384;          // FFT size
static constexpr size_t H_IN = N / 8;      // Input hop (analysis)
static constexpr size_t STRETCH = 20;       // Stretch factor
static constexpr size_t H_OUT = N / 4;     // Output hop (synthesis)
static constexpr size_t OUT_RING = 4 * N;
static constexpr size_t kStretchClearChunk = 4096;

static constexpr size_t kMicroLoopMaxSize = 16384;
// Ensure stretched buffer size is a multiple of H_OUT for proper circular OLA
static constexpr size_t kMicroLoopMaxStretchedSize = ((STRETCH * kMicroLoopMaxSize) / H_OUT) * H_OUT;

class MicroLooperModule : public BaseEffectModule
{
  public:
    MicroLooperModule();
    ~MicroLooperModule() override;

    enum Param {
      LOOP_MODE,
      SLICE,
      IN_MIX,
      BALANCE,
      SENSITIVITY,
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
    static float DSY_SDRAM_BSS stretched_buffer_a_[kMicroLoopMaxStretchedSize];
    static float DSY_SDRAM_BSS stretched_buffer_b_[kMicroLoopMaxStretchedSize];

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

    TapeModulator speed_error_generator_;

    void WriteBuffer(float in);
    void StartStretching();
    float ReadStretchedSample(size_t idx, bool normalized);

    // Stretching state
    bool is_stretching_ = false;
    bool streaming_stretch_ = false;   // True when stretching while still recording
    bool use_stretched_buffer_ = false;
    size_t stretch_read_pos_ = 0;      // Position in source buffer_
    size_t stretch_write_pos_ = 0;     // Position in stretched_buffer_
    size_t stretch_total_frames_ = 0;
    size_t stretch_frames_done_ = 0;
    PlayingHead stretch_playing_head_;
    bool stretch_clear_pending_ = false;
    size_t stretch_clear_pos_ = 0;
    float stretch_speed_ = 1.0f;

    // Double-buffering for stretched playback
    // active_stretch_buffer_: false = A is playing, true = B is playing
    // write_stretch_buffer_: false = writing to A, true = writing to B
    bool active_stretch_buffer_ = false;
    bool write_stretch_buffer_ = false;
    size_t stretched_length_a_ = 0;
    size_t stretched_length_b_ = 0;
    size_t stretched_ready_length_a_ = 0;
    size_t stretched_ready_length_b_ = 0;
    bool stretched_buffer_normalized_a_ = false;
    bool stretched_buffer_normalized_b_ = false;

    // ============================================================
    // AUTO-START (envelope follower + threshold + hysteresis)
    // ============================================================
    void UpdateEnv(float x_abs);
    void AutoStartLogic();

    // Envelope follower state
    float env_ = 0.0f;
    float a_att_ = 0.0f;   // attack coefficient
    float a_rel_ = 0.0f;   // release coefficient

    // Trigger state (sample counters)
    uint32_t above_count_ = 0;
    uint32_t below_count_ = 0;
    bool auto_armed_ = true;
    bool auto_start_enabled_ = true;  // master enable for auto-start

    // Auto-start parameters (defaults tuned for guitar)
    // Linear interpolation: thr = threshold_on_ + (threshold_on_min_ - threshold_on_) * sensitivity
    // At sensitivity=0.5: (0.055 + 0.005) / 2 = 0.03
    float threshold_on_ = 0.1f;     // threshold at sensitivity=0 (hard to trigger)
    float threshold_on_min_ = 0.001f; // threshold at sensitivity=1 (easy to trigger)
    float attack_ms_ = 8.0f;          // envelope attack time
    float release_ms_ = 200.0f;       // envelope release time
    float start_hold_ms_ = 10.0f;     // must stay above threshold_on_ this long
    float rearm_ms_ = 500.0f;         // must stay below threshold_off_ this long

    // Pre-computed sample counts (updated in Init)
    uint32_t start_hold_samps_ = 0;
    uint32_t rearm_samps_ = 0;
};

} // namespace bkshepherd
