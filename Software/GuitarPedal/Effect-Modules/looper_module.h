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
    //void ParameterChanged(int parameter_id) override;
    //void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
    float GetBrightnessForLED(int led_id) const override;

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
    float offset_ = 0.0f;
    static float DSY_SDRAM_BSS buffer_[kNumLayers][kMaxBufferSize];
    PlayingHead playing_head_;
    PlayingHead recording_head_;
    uint16_t prev_wrap_around_count_ = 0;
    bool one_pass_recording_ = false;
    void ClearTopLayers(size_t clear_from);
    void SquashLayers();
    void WriteBuffer(float in);
    void ResetBuffer();
  };
} // namespace bkshepherd
#endif
#endif