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
    void BlockPreProcessing(size_t size) override;
    void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
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
    int m_cachedClippingType = 0;
    float m_cachedIntensity = 0.0f;
    float m_cachedLevel = 1.0f;
    float m_cachedDryGain = 1.0f;
    float m_cachedWetGain = 0.0f;
};

static const float kDriveComp[128] = {
  1.000000f,
  0.830744f,
  0.675962f,
  0.553188f,
  0.456218f,
  0.379761f,
  0.319400f,
  0.271514f,
  0.233198f,
  0.202162f,
  0.176640f,
  0.155298f,
  0.137152f,
  0.121489f,
  0.107809f,
  0.095766f,
  0.085121f,
  0.075715f,
  0.067430f,
  0.060177f,
  0.053881f,
  0.048467f,
  0.043859f,
  0.039977f,
  0.036736f,
  0.034048f,
  0.031825f,
  0.029982f,
  0.028437f,
  0.027118f,
  0.025960f,
  0.024911f,
  0.023928f,
  0.022982f,
  0.022054f,
  0.021135f,
  0.020224f,
  0.019329f,
  0.018461f,
  0.017636f,
  0.016871f,
  0.016181f,
  0.015581f,
  0.015083f,
  0.014695f,
  0.014420f,
  0.014257f,
  0.014199f,
  0.014237f,
  0.014357f,
  0.014541f,
  0.014770f,
  0.015025f,
  0.015285f,
  0.015531f,
  0.015747f,
  0.015917f,
  0.016031f,
  0.016081f,
  0.016063f,
  0.015978f,
  0.015830f,
  0.015627f,
  0.015381f,
  0.015103f,
  0.014810f,
  0.014515f,
  0.014235f,
  0.013983f,
  0.013772f,
  0.013611f,
  0.013509f,
  0.013467f,
  0.013487f,
  0.013565f,
  0.013694f,
  0.013865f,
  0.014064f,
  0.014279f,
  0.014494f,
  0.014694f,
  0.014865f,
  0.014996f,
  0.015076f,
  0.015097f,
  0.015059f,
  0.014960f,
  0.014807f,
  0.014608f,
  0.014375f,
  0.014124f,
  0.013872f,
  0.013635f,
  0.013431f,
  0.013276f,
  0.013182f,
  0.013157f,
  0.013206f,
  0.013326f,
  0.013508f,
  0.013740f,
  0.014001f,
  0.014270f,
  0.014521f,
  0.014729f,
  0.014871f,
  0.014929f,
  0.014891f,
  0.014756f,
  0.014534f,
  0.014245f,
  0.013922f,
  0.013606f,
  0.013345f,
  0.013184f,
  0.013162f,
  0.013302f,
  0.013601f,
  0.014023f,
  0.014494f,
  0.014902f,
  0.015113f,
  0.014995f,
  0.014472f,
  0.013611f,
  0.012754f,
  0.012713f,
  0.012046f
};

} // namespace bkshepherd
#endif
#endif
