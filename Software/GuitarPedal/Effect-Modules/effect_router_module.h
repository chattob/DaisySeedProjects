#pragma once
#ifndef EFFECT_ROUTER_MODULE_H
#define EFFECT_ROUTER_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include <stdint.h>
#ifdef __cplusplus

/** @file effect_router_module.h */

namespace bkshepherd {

class EffectRouterModule : public BaseEffectModule {
  public:
    EffectRouterModule();
    ~EffectRouterModule();

    void Init(float sample_rate) override;
    void SetInner(BaseEffectModule* inner);

    // Called from switchRoutes when triswitch_0_left goes to ON
    void AlternateFootswitchPressed() override;

    // Called from switchRoutes when triswitch_0_left leaves ON
    void AlternateFootswitchReleased() override;

    bool Poll() override;

    void ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) override;

  private:
    BaseEffectModule* inner_ = nullptr;
    bool              tri_left_active_ = false;
    float             sample_rate_;

    inline void CopyStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
    {
        // Simple pass-through if in/out are different
        if(in == out)
            return;

        const float* inL  = in[0];
        const float* inR  = in[1];
        float* outL = out[0];
        float* outR = out[1];

        for(size_t i = 0; i < size; ++i)
        {
            outL[i] = inL[i];
            outR[i] = inR[i];
        }
    }
};
} // namespace bkshepherd
#endif
#endif