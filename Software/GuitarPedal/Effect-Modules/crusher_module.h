#pragma once
#ifndef CRUSHER_MODULE_H
#define CRUSHER_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include "../Util/XorShift32.h"
#include <q/fx/biquad.hpp>
#include <stdint.h>
#ifdef __cplusplus

/** @file crusher_module.h */

using namespace daisysp;

namespace bkshepherd {

class Bitcrusher {
  public:
    void Init(float sample_rate) {
        quant        = 65536.0f;
        sampleRate   = sample_rate;
        targetRate   = sample_rate;
        phaseAccum   = 0.0f;
        heldSample   = 0.0f;
        jitterAmount = 0.0f;

        nextInterval = 1.0f; // will be set on first Process or when targetRate changes
    }

    float Process(float in) {
        // base interval in input-sample counts
        float base = sampleRate / targetRate;
        if(base < 1.0f) base = 1.0f;

        // ensure nextInterval is initialized/sane
        if(nextInterval < 1.0f) nextInterval = base;

        phaseAccum += 1.0f; // count input samples
        if(phaseAccum >= nextInterval) {
            phaseAccum -= nextInterval;

            // quantize (rounding is usually nicer than truncation)
            heldSample = nearbyintf(in * quant) / quant;

            // schedule next update with jitter
            float j = rng.randSigned(); // [-1, +1]
            float interval = base * (1.0f + 0.25f * jitterAmount * j);

            // clamp to avoid extremes
            if(interval < 1.0f) interval = 1.0f;
            float maxInterval = base * 4.0f;
            if(interval > maxInterval) interval = maxInterval;

            nextInterval = interval;
        }
        return heldSample;
    }

    void setTargetSampleRate(float rate) {
        if(rate < 100.0f) rate = 100.0f;
        if(rate > sampleRate) rate = sampleRate;
        targetRate = rate;

        // reset scheduling so changes take effect immediately and cleanly
        float base = sampleRate / targetRate;
        if(base < 1.0f) base = 1.0f;
        nextInterval = base;
        if(phaseAccum > nextInterval) phaseAccum = nextInterval;
    }

    void setJitter(float amount) { jitterAmount = (amount < 0.0f) ? 0.0f : amount; }

    void setNumberOfBits(float nBits) {
        if(nBits < 1.0f) nBits = 1.0f;
        if(nBits > 32.0f) nBits = 32.0f;
        quant = powf(2.0f, nBits);
    }

  private:
    float quant, sampleRate, targetRate;
    float phaseAccum, heldSample;
    float jitterAmount;
    float nextInterval;
    XorShift32 rng;
};


class CrusherModule : public BaseEffectModule {
  public:
    CrusherModule();
    ~CrusherModule();

    enum Param {
      LEVEL = 0,
      BITS,
      RATE,
      JITTER,
      CUTOFF,
      MIX,
      PARAM_COUNT
    };

    void Init(float sample_rate) override;
    void ProcessMonoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) override;
    void ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) override;

  private:
    Bitcrusher m_bitcrusherL;
    Bitcrusher m_bitcrusherR;

    float m_rateMin;
    float m_rateMax;
    float m_cutoffMin;
    float m_cutoffMax;

    cycfi::q::lowpass lp_filter_;
};
} // namespace bkshepherd
#endif
#endif
