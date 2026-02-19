#pragma once
#ifndef DISTORTION_MODULE_H
#define DISTORTION_MODULE_H

#include <stdint.h>

#include "base_effect_module.h"
#include "daisysp.h"

#ifdef __cplusplus

/** @file distortion_module.h */

using namespace daisysp;

namespace bkshepherd {

constexpr uint8_t oversamplingFactor = 4;
constexpr float preFilterCutoffBase = 140.0f;
constexpr float preFilterCutoffMax = 300.0f;
constexpr float postFilterCutoff = 8000.0f;

class DistortionModule : public BaseEffectModule {
  public:
    DistortionModule();
    ~DistortionModule();

    enum Param {
        LEVEL = 0,
        GAIN,
        TONE,
        DIST_TYPE,
        INTENSITY,
        OVERSAMP,
        MIX,
        PARAM_COUNT
    };

    void Init(float sample_rate) override;
    void ParameterChanged(int parameter_id) override;
    void ProcessMonoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)  override;
    void ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) override;
    float GetBrightnessForLED(int led_id) const override;

  private:
    void InitializeFilters();
    float hardClipping(float input, float threshold);
    float diodeClipping(float input, float threshold);
    float softClipping(float input, float gain);
    float fuzzEffect(float input, float intensity);
    float tubeSaturation(float input, float gain);
    float multiStage(float sample, float env);
    float dynamicPreFilterCutoff(float inputEnergy);
    void processDistortion(float &sample, const int &clippingType, const float &intensity, float env);
    void normalizeVolume(float &sample, int clippingType);
    float ProcessSample(float input, int clippingType, float intensity, int channel);

    float m_levelMin = 0.0f;
    float m_levelMax = 1.0f;

    float m_gainMin = 1.0f;
    float m_gainMax = 8.0f;

    Tone m_tone;

    bool m_oversampling;
    float m_os_buffer[2][oversamplingFactor];  // per-channel workspace for oversampling
    float m_env[2] = {0.0f, 0.0f};
    float m_pre_cutoff[2] = {preFilterCutoffBase, preFilterCutoffBase};
};
} // namespace bkshepherd
#endif
#endif
