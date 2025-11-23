#pragma once
#ifndef FILTER_MODULE_H
#define FILTER_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include <stdint.h>
#ifdef __cplusplus

/** @file filter_module.h */

namespace bkshepherd {

class FilterModule : public BaseEffectModule {
    public:
        FilterModule();
        ~FilterModule();

        void Init(float sample_rate) override;
        void ProcessStereo(float inL, float inR) override;
        float GetBrightnessForLED(int led_id) const override;

    private:
        daisysp::Svf sv_filter_;
        float filter_range_;

};
} // namespace bkshepherd
#endif
#endif
