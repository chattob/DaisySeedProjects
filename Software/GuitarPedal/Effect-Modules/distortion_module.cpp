#include "distortion_module.h"
#include <algorithm>
#include <q/fx/biquad.hpp>

using namespace bkshepherd;

static const char *s_clippingOptions[6] = {"Hard Clip", "Soft Clip", "Fuzz", "Tube", "Multi Stage", "Diode Clip"};

cycfi::q::highpass preFilter(preFilterCutoffBase, 48000); // Dummy values that get overwritten in Init
cycfi::q::lowpass postFilter(postFilterCutoff, 48000);    // Dummy values that get overwritten in Init
cycfi::q::lowpass upsamplingLowpassFilter(0.0f, 48000);   // Dummy values that get overwritten in Init

static const int s_paramCount = 7;
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
    preFilter.config(preFilterCutoffBase, GetSampleRate());

    if (m_oversampling) {
        postFilter.config(postFilterCutoff, GetSampleRate() * oversamplingFactor);
    } else {
        postFilter.config(postFilterCutoff, GetSampleRate());
    }

    upsamplingLowpassFilter.config(GetSampleRate() / (2.0f * static_cast<float>(oversamplingFactor)), GetSampleRate());
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

float DistortionModule::multiStage(float sample, float drive) {
    // Clamp to a sane range
    const float g = drive;//std::clamp(drive, 0.0f, 4.0f);

    // Internal stage gains: all moderate, decreasing slightly per stage
    const float d1 = 0.80f * g;  // first stage: main shaping
    const float d2 = 0.67f * g;  // second stage: additional compression
    const float d3 = 0.56f * g;  // third stage: "power amp"

    // Envelope-based bias: no bias for very low levels
    float env       = m_env;               // from existing envelope follower
    float env_norm  = env / 0.2f;          // 0.2 ≈ "pretty loud", tweak by ear
    env_norm        = std::clamp(env_norm, 0.0f, 1.0f);

    // Small level-dependent bias for asymmetry -> even harmonics, warmer feel
    const float bias = 0.005f * env_norm;

    // Stage 1: gentle soft clip with slight positive bias
    float s1 = softClipping(sample + bias, d1);

    // Stage 2: another soft clip, partially recentre around zero
    float s2 = softClipping(s1 - 0.5f * bias, d2);

    // Stage 3: atan as "power amp" stage
    float s3 = tubeSaturation(s2, d3);

    return s3;
}

float DistortionModule::dynamicPreFilterCutoff(float inputEnergy) {
    return preFilterCutoffBase + (preFilterCutoffMax - preFilterCutoffBase) * std::tanh(inputEnergy);
}

void DistortionModule::processDistortion(float &sample,           // Sample to process
                        const float &gain,       // Gain
                        const int &clippingType, // Clipping type
                        const float &intensity  // Intensity
                        ) {
    sample *= gain;

    switch (clippingType) {
    case 0: // Hard Clipping
        sample = hardClipping(sample, 1.0f - intensity);
        break;
    case 1: // Soft Clipping
        sample = softClipping(sample, gain);
        break;
    case 2: // Fuzz
        sample = fuzzEffect(sample, intensity * 10.0f);
        break;
    case 3: // Tube Saturation
        sample = tubeSaturation(sample, intensity * 10.0f);
        break;
    case 4: // Multi-stage
        sample = multiStage(sample, gain);
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
        sample *= 0.5f;
        break;
    case 5: // Diode Clipping
        sample *= 1.8f;
        break;
    }
}

void DistortionModule::ProcessMonoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    if (m_isEnabled) {
        const float gain = m_gainMin + (GetParameterAsFloat(GAIN) * (m_gainMax - m_gainMin));
        const int clippingType = GetParameterAsBinnedValue(DIST_TYPE) - 1;
        const float intensity = GetParameterAsFloat(INTENSITY);
        const float level = m_levelMin + (GetParameterAsFloat(LEVEL) * (m_levelMax - m_levelMin));
        float mix = GetParameterAsFloat(MIX);

        // constant-power crossfade
        const float a = sqrtf(1.0f - mix);
        const float b = sqrtf(mix);

        for (size_t i = 0; i < size; i++) {
            float distorted = in[0][i];

            // Apply high-pass filter to remove excessive low frequencies
            const float energy = std::abs(distorted);
            m_env += 0.01f * (energy - m_env); // simple envelope follower

            const float target_cutoff = dynamicPreFilterCutoff(m_env);
            if (std::abs(target_cutoff - m_pre_cutoff) > 10.0f)  // only if changed enough
            {
                m_pre_cutoff = target_cutoff;
                preFilter.config(m_pre_cutoff, GetSampleRate());
            }

            distorted = preFilter(distorted);

            // Reduce signal amplitude before clipping
            distorted = distorted * 0.5f;

            if (m_oversampling) {
                // zero-stuff oversampling of a single sample
                for (int j = 0; j < oversamplingFactor; ++j)
                {
                    float os_sample = (j == 0) ? distorted : 0.0f;

                    // interpolate with low-pass
                    os_sample = upsamplingLowpassFilter(os_sample);

                    // nonlinear + post-filter
                    processDistortion(os_sample, gain, clippingType, intensity);
                    os_sample = postFilter(os_sample);

                    m_os_buffer[j] = os_sample;
                }

                // simplest: take first sample, or better: average
                float acc = 0.0f;
                for (int j = 0; j < oversamplingFactor; ++j)
                    acc += m_os_buffer[j];
                distorted = acc / float(oversamplingFactor);
            }
            else
            {
                processDistortion(distorted, gain, clippingType, intensity);
                distorted = postFilter(distorted);
            }

            // Normalize the volume between the types of distortion
            normalizeVolume(distorted, clippingType);

            // Apply tilt-tone filter
            const float filter_out = ProcessTiltToneControl(distorted);

            const float clean = in[0][i];
            const float wet   = filter_out * level;

            out[0][i]  = a * clean + b * wet;
            out[1][i] = out[0][i];
        }
    }
}

void DistortionModule::ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    // Calculate the mono effect
    ProcessMonoBlock(in, out, size);
}

float DistortionModule::ProcessTiltToneControl(float input) {
    volatile const float toneAmount = GetParameterAsFloat(TONE);

    // Process input with one-pole low-pass
    const float lp = m_tone.Process(input);

    // Compute the high-passed portion
    const float hp = input - lp;

    // Crossfade: toneAmount=0 => all LP (more bass), toneAmount=1 => all HP (more treble)
    return lp * (1.f - toneAmount) + hp * toneAmount;
}

float DistortionModule::GetBrightnessForLED(int led_id) const {
    float value = BaseEffectModule::GetBrightnessForLED(led_id);

    return value;
}