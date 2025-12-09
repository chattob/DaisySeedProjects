#pragma once

#include "base_effect_module.h"
#include <q/fx/biquad.hpp>

namespace bkshepherd
{

class FilterModule : public BaseEffectModule
{
  public:
    FilterModule();
    ~FilterModule() override;

    enum Param {
        CUTOFF = 0,
        HP_MODE,
        PARAM_COUNT
    };

    void Init(float sample_rate) override;
    void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
    void ParameterChanged(int parameter_id) override;

    float GetBrightnessForLED(int led_id) const override;

  private:
    void UpdateFilters();

    // Parameters
    float cutoff_norm_;   // 0..1
    bool  hp_mode_;       // false = LP, true = HP

    // Range for cutoff (Hz)
    float cutoff_min_;
    float cutoff_max_;

    // Filters (mono)
    cycfi::q::highpass hp_filter_;
    cycfi::q::lowpass  lp_filter_;
};

} // namespace bkshepherd