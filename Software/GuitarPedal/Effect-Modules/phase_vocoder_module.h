#pragma once
#ifndef PHASE_VOCODER_MODULE_H
#define PHASE_VOCODER_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include "clouds/dsp/pvoc/phase_vocoder.h"
#include "clouds/dsp/parameters.h"
#include <stdint.h>
#ifdef __cplusplus

/** @file phase_vocoder_module.h */

using namespace daisysp;

namespace bkshepherd {

class PhaseVocoderModule : public BaseEffectModule {
  public:
    PhaseVocoderModule();
    ~PhaseVocoderModule();

    void ProcessStereo(float inL, float inR) override;
  
  private:
    clouds::PhaseVocoder phase_vocoder_;
    clouds::Parameters parameters_;
};
} // namespace bkshepherd
#endif
#endif