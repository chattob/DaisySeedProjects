#pragma once
#ifndef TAPE_MODULE_H
#define TAPE_MODULE_H

#include "base_effect_module.h"
#include "../Util/tape_modulator.h"
#include "daisysp.h"
#include <stdint.h>

#ifdef __cplusplus

using namespace daisysp;

namespace bkshepherd {

class TapeModule : public BaseEffectModule {
  public:
    TapeModule();
    ~TapeModule() override;

    enum Param {
        DEPTH = 0,
        RATE,
        MIX,
        PARAM_COUNT
    };

    void Init(float sample_rate) override;
    void ParameterChanged(int parameter_id) override;
    void ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) override;

  private:
    float m_currentDelayLeft;
    float m_currentDelayRight;
    float m_wet;
    float m_dry;

    TapeModulator m_modTapeLeft;
    TapeModulator m_modTapeRight;
};

} // namespace bkshepherd

#endif
#endif
