#pragma once
#ifndef LOOPER_MODULE_H
#define LOOPER_MODULE_H

#include "base_effect_module.h"
#include "../Util/playing_head.h"
#include "../constants.h"
#include "daisysp.h"
#include <stdint.h>
#ifdef __cplusplus

/** @file looper_module.h */

static constexpr int kNumLeds        = 2;
static constexpr int kMaxBlinkSteps  = 10;

namespace bkshepherd {

class LooperModule : public BaseEffectModule {
  public:
    LooperModule();
    ~LooperModule();

    enum Param {
        LAYER = 0,
        FADING,
        SPEED,
        SLICE,
        PARAM_COUNT
    };

    void BypassFootswitchPressed() override;
    void AlternateFootswitchPressed() override;
    void AlternateFootswitchHeldFor1Second() override;
    void FootswitchPressed(size_t footswitch_id) override;
    void FootswitchReleased(size_t footswitch_id) override;
    //void ParameterChanged(int parameter_id) override;
    //void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
    float GetBrightnessForLED(int led_id) const override;
    void SetParameterAsMagnitude(int parameter_id, float value) override;

  private:
    float output_;
    size_t loop_length_ = 0;
    float loop_length_f_ = 0.0f;
    size_t mod = 0;
    bool is_recording_ = false;
    size_t recording_layer_ = 0;
    bool is_playing_ = false;
    bool first_layer_ = false;
    size_t n_recorded_layers_ = 0;
    size_t selected_layer_ = 0;
    float prev_layer_knob_val_ = 0.0f;
    bool modifier_on_ = false;
    bool fixed_lenght_mode_ = false;
    float offset_ = 0.0f;
    static float DSY_SDRAM_BSS buffer_[kNumLayers][kMaxBufferSize];
    PlayingHead playing_head_;
    PlayingHead recording_head_;

    mutable float   pattern_brightness_[kNumLeds]             = {0.0f, 0.0f};    
    mutable bool    blink_[kNumLeds]                          = {false, false};
    mutable int8_t  blink_pattern_[kNumLeds][kMaxBlinkSteps]  = {};
    mutable uint8_t blink_tracker_[kNumLeds]                  = {0, 0};
    mutable uint16_t prev_wrap_around_count_                  = 0;
    mutable uint32_t next_toggle_time_[kNumLeds]              = {0, 0};
    mutable size_t  prev_selected_layer_                      = static_cast<size_t>(-1);

    void ClearTopLayers(size_t clear_from);
    void SquashLayers();
    void WriteBuffer(float in);
    void ResetBuffer();
  };
} // namespace bkshepherd
#endif
#endif