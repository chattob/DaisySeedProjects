#pragma once
#ifndef CRUSHER_MODULE_H
#define CRUSHER_MODULE_H

#include "base_effect_module.h"
#include "daisysp.h"
#include <q/fx/biquad.hpp>
#include <stdint.h>
#ifdef __cplusplus

/** @file crusher_module.h */

using namespace daisysp;

namespace bkshepherd {

class Bitcrusher {
  public:
    Bitcrusher() {}
    ~Bitcrusher() {}

    void Init(float sample_rate) {
        quant = 65536.0f;
        sampleRate = sample_rate;
        targetRate = sample_rate;
        phaseAccum = 0.0f;
        heldSample = 0.0f;
    }

    // Sample rate reduction uses a phase accumulator to allow non-integer
    // downsample ratios and smooth transitions when modulating the rate
    // (e.g., from a pitch detector). Each sample, we accumulate targetRate.
    // When it exceeds sampleRate, we grab and crush a new sample.
    float Process(float in) {
        phaseAccum += targetRate;
        if (phaseAccum >= sampleRate) {
            phaseAccum -= sampleRate;
            heldSample = truncf(in * quant) / quant;
        }
        return heldSample;
    }

    void setNumberOfBits(float nBits) {
        if (nBits < 1.0f) {
            nBits = 1.0f;
        } else if (nBits > 32.0f) {
            nBits = 32.0f;
        }
        quant = powf(2.0f, nBits);
    }

    void setTargetSampleRate(float rate) {
        if (rate < 100.0f) {
            rate = 100.0f;
        } else if (rate > sampleRate) {
            rate = sampleRate;
        }
        targetRate = rate;
    }

    float getTargetSampleRate() const { return targetRate; }
    float getSampleRate() const { return sampleRate; }

  private:
    float quant;
    float sampleRate;
    float targetRate;
    float phaseAccum;
    float heldSample;
};

class CrusherModule : public BaseEffectModule {
  public:
    CrusherModule();
    ~CrusherModule();

    enum Param {
      LEVEL = 0,
      BITS,
      RATE,
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
