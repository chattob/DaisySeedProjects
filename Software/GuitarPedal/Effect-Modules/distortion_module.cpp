#include "distortion_module.h"
#include <algorithm>
#include <q/fx/biquad.hpp>

using namespace bkshepherd;

static const char *s_clippingOptions[6] = {"Hard Clip", "Soft Clip", "Fuzz", "Tube", "Multi Stage", "Diode Clip"};

cycfi::q::highpass preFilter[2] = {
    cycfi::q::highpass(preFilterCutoffBase, 48000), // Dummy values that get overwritten in Init
    cycfi::q::highpass(preFilterCutoffBase, 48000)
};
cycfi::q::lowpass postFilter[2] = {
    cycfi::q::lowpass(postFilterCutoff, 48000), // Dummy values that get overwritten in Init
    cycfi::q::lowpass(postFilterCutoff, 48000)
};
cycfi::q::lowpass upsamplingLowpassFilter[2] = {
    cycfi::q::lowpass(0.0f, 48000), // Dummy values that get overwritten in Init
    cycfi::q::lowpass(0.0f, 48000)
};

static constexpr int s_paramCount = DistortionModule::PARAM_COUNT;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Level",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 0,
        midiCCMapping : -1
    },
    {
        name : "Gain",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 1,
        midiCCMapping : -1,
    },
    {
        name : "Tone",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 2,
        midiCCMapping : -1,
    },
    {
        name : "Dist Type",
        valueType : ParameterValueType::Binned,
        valueBinCount : 6,
        valueBinNames : s_clippingOptions,
        defaultValue : {.uint_value = 0},
        knobMapping : 3,
        midiCCMapping : -1
    },
    {
        name : "Intensity",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 4,
        midiCCMapping : -1,
    },
    {
        name : "Oversamp",
        valueType : ParameterValueType::Bool,
        valueBinCount : 0,
        defaultValue : {.uint_value = 1},
        knobMapping : 5,
        midiCCMapping : -1
    },
    {
        name : "Mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : 6,
        midiCCMapping : -1
    }
};

// Fast tan^-1 approximation
inline float fast_atan(float x) {
    x = std::clamp(x, -3.0f, 3.0f); // clamp to valid approximation range
    const float x2 = x * x;
    return x * (1.0f + 0.280872f * x2) / (1.0f + 0.580581f * x2); // rational 3rd-order polynomial fit
}

// Fast tanh approximation
inline float fast_tanh(float x) {
    x = std::clamp(x, -3.0f, 3.0f); // clamp to valid approximation range
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2); // rational 3rd-order polynomial fit
}

// Default Constructor
DistortionModule::DistortionModule() : BaseEffectModule() {
    // Set the name of the effect
    m_name = "Distortion";

    // Setup the meta data reference for this Effect
    m_paramMetaData = s_metaData;

    // Initialize Parameters for this Effect
    this->InitParams(s_paramCount);
}

// Destructor
DistortionModule::~DistortionModule() {
    // No Code Needed
}

void DistortionModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
    m_tone.Init(sample_rate);

    // Pivot between 500 Hz and 2 kHz as the tone amount changes
    m_tone.SetFreq(500.0f + 1500.0f * GetParameterAsFloat(TONE));

    m_oversampling = GetParameterAsBool(OVERSAMP);
    InitializeFilters();
}

void DistortionModule::InitializeFilters() {
    for (int ch = 0; ch < 2; ch++) {
        preFilter[ch].config(preFilterCutoffBase, GetSampleRate());

        if (m_oversampling) {
            postFilter[ch].config(postFilterCutoff, GetSampleRate() * oversamplingFactor);
        } else {
            postFilter[ch].config(postFilterCutoff, GetSampleRate());
        }

        upsamplingLowpassFilter[ch].config(GetSampleRate() / (2.0f * static_cast<float>(oversamplingFactor)), GetSampleRate());
    }
}

void DistortionModule::ParameterChanged(int parameter_id) {
    if (parameter_id == 5) {
        m_oversampling = GetParameterAsBool(OVERSAMP);
        InitializeFilters();
    } else if (parameter_id == 2) {
        // Pivot between 500 Hz and 2 kHz as the tone amount changes
        m_tone.SetFreq(500.0f + 1500.0f * GetParameterAsFloat(TONE));
    }
}

float DistortionModule::hardClipping(float input, float threshold) {
    return std::clamp(input, -threshold, threshold);
}

float DistortionModule::diodeClipping(float input, float threshold) {
    if (input > threshold)
        return threshold - std::exp(-(input - threshold));
    else if (input < -threshold)
        return -threshold + std::exp(input + threshold);
    return input;
}

float DistortionModule::softClipping(float input, float gain) {
    return fast_tanh(input * gain);
}

float DistortionModule::fuzzEffect(float input, float intensity) {
    // Symmetrical clipping with extreme compression
    float fuzzed = softClipping(input, intensity);

    // Introduce a slight asymmetry for a classic fuzz character and adds harmonic content
    fuzzed += 0.05f * std::sin(input * 20.0f);

    // Dynamic response: Adjust the intensity based on the input signal's amplitude
    const float dynamicIntensity = intensity * (1.0f + 0.5f * std::abs(input));
    fuzzed = softClipping(fuzzed, dynamicIntensity);

    return fuzzed;
}

float DistortionModule::tubeSaturation(float input, float gain) {
    return fast_atan(input * gain);
}

float DistortionModule::multiStage(float sample, float env) {
    const float gain = GetParameterAsFloat(GAIN);
    const float g = m_gainMin + (gain * (m_gainMax - m_gainMin));

    sample *= g;

    // Internal stage gains: all moderate, decreasing slightly per stage
    const float d1 = 0.80f;  // first stage: main shaping
    const float d2 = 0.67f;  // second stage: additional compression
    const float d3 = 0.56f;  // third stage: "power amp"

    // Envelope-based bias: no bias for very low levels
    float env_norm  = env / 0.2f;          // 0.2 ≈ "pretty loud", tweak by ear
    env_norm        = std::clamp(env_norm, 0.0f, 1.0f);

    // Small level-dependent bias for asymmetry -> even harmonics, warmer feel
    const float bias = 0.005f * env_norm;

    // Stage 1: gentle soft clip with slight positive bias
    float s1 = softClipping(sample + bias, d1 * g);

    // Stage 2: another soft clip, partially recentre around zero
    float s2 = softClipping(s1 - 0.5f * bias, d2 * g);

    // Stage 3: atan as "power amp" stage
    float s3 = tubeSaturation(s2, d3 * g);

    // Per-step compensation table aligned to MIDI CC gain steps (0..127).
    const int driveCompIdx = std::clamp(static_cast<int>(gain * 127.0f + 0.5f), 0, 127);
    return s3 * kDriveComp[driveCompIdx];
}

float DistortionModule::dynamicPreFilterCutoff(float inputEnergy) {
    return preFilterCutoffBase + (preFilterCutoffMax - preFilterCutoffBase) * fast_tanh(inputEnergy);
}

void DistortionModule::processDistortion(float &sample,           // Sample to process
                        const int &clippingType, // Clipping type
                        const float &intensity, // Intensity
                        float env
                        ) {
    switch (clippingType) {
    case 0: // Hard Clipping
        sample = hardClipping(sample, 1.0f - intensity);
        break;
    case 2: // Fuzz
        sample = fuzzEffect(sample, intensity * 10.0f);
        break;
    case 3: // Tube Saturation
        sample = tubeSaturation(sample, intensity * 10.0f);
        break;
    case 4: // Multi-stage
        sample = multiStage(sample, env);
        break;
    case 5: // Diode Clipping
        sample = hardClipping(sample, 1.0f - intensity);
        break;
    }
}

void DistortionModule::normalizeVolume(float &sample, int clippingType) {
    switch (clippingType) {
    case 0: // Hard Clipping
        sample *= 1.8f;
        break;
    case 1: // Soft Clipping
        sample *= 0.8f;
        break;
    case 2: // Fuzz
        sample *= 1.0f;
        break;
    case 3: // Tube Saturation
        sample *= 0.9f;
        break;
    case 4: // Multi-stage
        break;
    case 5: // Diode Clipping
        sample *= 1.8f;
        break;
    }
}

float DistortionModule::ProcessSample(float input, int clippingType, float intensity, int channel) {
    float distorted = input;

    // Channel-specific envelope and cutoff tracking to avoid L/R crosstalk.
    const float energy = std::abs(distorted);
    m_env[channel] += 0.01f * (energy - m_env[channel]);

    const float target_cutoff = dynamicPreFilterCutoff(m_env[channel]);
    if (std::abs(target_cutoff - m_pre_cutoff[channel]) > 10.0f) {
        m_pre_cutoff[channel] = target_cutoff;
        preFilter[channel].config(m_pre_cutoff[channel], GetSampleRate());
    }

    distorted = preFilter[channel](distorted);

    if (m_oversampling) {
        // Zero-stuff oversampling of a single sample.
        for (int j = 0; j < oversamplingFactor; ++j) {
            float os_sample = (j == 0) ? distorted : 0.0f;

            // Interpolate with low-pass.
            os_sample = upsamplingLowpassFilter[channel](os_sample);

            // Nonlinear + post-filter.
            processDistortion(os_sample, clippingType, intensity, m_env[channel]);
            os_sample = postFilter[channel](os_sample);

            m_os_buffer[channel][j] = os_sample;
        }

        float acc = 0.0f;
        for (int j = 0; j < oversamplingFactor; ++j) {
            acc += m_os_buffer[channel][j];
        }
        distorted = acc / float(oversamplingFactor);
    } else {
        processDistortion(distorted, clippingType, intensity, m_env[channel]);
        distorted = postFilter[channel](distorted);
    }

    normalizeVolume(distorted, clippingType);
    return distorted;
}

void DistortionModule::BlockPreProcessing(size_t size) {
    (void)size;

    const float mix = GetParameterAsFloat(MIX);
    m_cachedClippingType = GetParameterAsBinnedValue(DIST_TYPE) - 1;
    m_cachedIntensity = GetParameterAsFloat(INTENSITY);
    m_cachedLevel = m_levelMin + (GetParameterAsFloat(LEVEL) * (m_levelMax - m_levelMin));
    m_cachedDryGain = sqrtf(1.0f - mix);
    m_cachedWetGain = sqrtf(mix);
}

void DistortionModule::ProcessMono(float in) {
    if (!m_isEnabled) {
        m_audioLeft = in;
        m_audioRight = in;
        return;
    }

    float distorted = ProcessSample(in, m_cachedClippingType, m_cachedIntensity, 0);
    const float wet = distorted * m_cachedLevel;
    m_audioLeft = m_cachedDryGain * in + m_cachedWetGain * wet;
    m_audioRight = m_audioLeft;
}

void DistortionModule::ProcessStereo(float inL, float inR) {
    if (!m_isEnabled) {
        m_audioLeft = inL;
        m_audioRight = inR;
        return;
    }

    float distortedL = ProcessSample(inL, m_cachedClippingType, m_cachedIntensity, 0);
    float distortedR = ProcessSample(inR, m_cachedClippingType, m_cachedIntensity, 1);

    const float wetL = distortedL * m_cachedLevel;
    const float wetR = distortedR * m_cachedLevel;

    m_audioLeft = m_cachedDryGain * inL + m_cachedWetGain * wetL;
    m_audioRight = m_cachedDryGain * inR + m_cachedWetGain * wetR;
}

float DistortionModule::GetBrightnessForLED(int led_id) const {
    float value = BaseEffectModule::GetBrightnessForLED(led_id);

    return value;
}
