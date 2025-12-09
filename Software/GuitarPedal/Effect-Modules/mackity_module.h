#pragma once
#ifndef MACKITY_MODULE_H
#define MACKITY_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include <stdint.h>
#ifdef __cplusplus

/** @file mackity_module.h */

namespace bkshepherd {

class MackityModule : public BaseEffectModule {
    public:
        MackityModule();
        ~MackityModule();

        enum Param {
            IN_TRIM = 0,
            OUT_PAD,
            MIX,
            PARAM_COUNT
        };

        void Init(float sample_rate) override;
        void ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) override;

    private:
        float sample_rate_;
        
        // biquad state: [0]=freq, [1]=Q, [2..6]=coeffs, [7..10]=L state, [11..14]=R state
        double  biquadA_[15]{};
        double  biquadB_[15]{};

        double  iirSampleAL_ = 0.0;
        double  iirSampleAR_ = 0.0;
        double  iirSampleBL_ = 0.0;
        double  iirSampleBR_ = 0.0;

        uint32_t fpdL_ = 1;
        uint32_t fpdR_ = 1;
};
} // namespace bkshepherd
#endif
#endif
