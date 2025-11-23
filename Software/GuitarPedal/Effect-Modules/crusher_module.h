#pragma once
#ifndef CRUSHER_MODULE_H
#define CRUSHER_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include <stdint.h>
#ifdef __cplusplus

/** @file crusher_module.h */

using namespace daisysp;

namespace bkshepherd {

class CrusherModule : public BaseEffectModule {
  public:
    CrusherModule();
    ~CrusherModule();

    void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
};
} // namespace bkshepherd
#endif
#endif