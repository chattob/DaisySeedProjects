#include "crusher_module.h"
#include "../Util/audio_utilities.h"

using namespace bkshepherd;

static constexpr int s_paramCount = CrusherModule::PARAM_COUNT;
static const ParameterMetaData s_metaData[s_paramCount] = {{
                                                               name : "Level",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 0.3f},
                                                               knobMapping : 0,
                                                               midiCCMapping : -1
                                                           },
                                                           {
                                                               name : "Bits",
                                                               valueType : ParameterValueType::Binned,
                                                               valueBinCount : 32,
                                                               defaultValue : {.uint_value = 32},
                                                               knobMapping : 1,
                                                               midiCCMapping : -1
                                                           },
                                                           {
                                                               name : "Rate",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 1.0f},
                                                               knobMapping : 2,
                                                               midiCCMapping : -1
                                                           },
                                                           {
                                                               name : "Jitter",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 0.0f},
                                                               knobMapping : -1,
                                                               midiCCMapping : -1
                                                           },
                                                           {
                                                               name : "Cutoff",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 0.5f},
                                                               knobMapping : 3,
                                                               midiCCMapping : -1
                                                           },
                                                           {
                                                               name : "Mix",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 1.0f},
                                                               knobMapping : 4,
                                                               midiCCMapping : -1
                                                           },
                                                           {
                                                               name : "Filter Q",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 0.0f},
                                                               knobMapping : -1,
                                                               midiCCMapping : -1
                                                           },
                                                           {
                                                               name : "Crunch Sub",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 0.0f},
                                                               knobMapping : -1,
                                                               midiCCMapping : -1
                                                           }};

// Default Constructor
CrusherModule::CrusherModule() : BaseEffectModule(), m_rateMin(100.0f), m_rateMax(48000.0f),
m_cutoffMin(500), m_cutoffMax(20000), m_filterQMin(0.707f), m_filterQMax(10.0f),
m_lpFilter{cycfi::q::lowpass(m_cutoffMax, 48000.0f), cycfi::q::lowpass(m_cutoffMax, 48000.0f)} {

    // Set the name of the effect
    m_name = "Crusher";

    // Setup the meta data reference for this Effect
    m_paramMetaData = s_metaData;

    // Initialize Parameters for this Effect
    this->InitParams(s_paramCount);
}

// Destructor
CrusherModule::~CrusherModule() {
    // No Code Needed
}

void CrusherModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
    m_rateMax = sample_rate;

    m_bitcrusherL.Init(sample_rate);
    m_bitcrusherR.Init(sample_rate);
    m_octaveGen.Init(sample_rate / static_cast<float>(resample_factor));
    for (size_t i = 0; i < resample_factor; ++i) {
        m_crunchBuf[i] = 0.0f;
        m_crunchUp[i] = 0.0f;
    }
    m_crunchIndex = 0;
}

void CrusherModule::BlockPreProcessing(size_t size) {
    (void)size;

    float cutoff = m_cutoffMin + GetParameterAsFloat(CUTOFF) * (m_cutoffMax - m_cutoffMin);
    float bits = (float)GetParameterAsBinnedValue(BITS);
    float t = GetParameterAsFloat(RATE);     // 0..1
    float rate = m_rateMin * powf(m_rateMax / m_rateMin, t);
    float jitter = GetParameterAsFloat(JITTER);
    float q = m_filterQMin + GetParameterAsFloat(FILTER_Q) * (m_filterQMax - m_filterQMin);
    auto gains = EnergyCrossfade(GetParameterAsFloat(MIX));

    m_cachedLevel = GetParameterAsFloat(LEVEL);
    m_cachedSub = GetParameterAsFloat(CRUNCH_SUB);
    m_cachedDryGain = gains.dry;
    m_cachedWetGain = gains.wet;

    m_lpFilter[0].config(cutoff, m_rateMax, q);
    m_lpFilter[1].config(cutoff, m_rateMax, q);

    m_bitcrusherL.setNumberOfBits(bits);
    m_bitcrusherL.setTargetSampleRate(rate);
    m_bitcrusherL.setJitter(jitter);
    m_bitcrusherR.setNumberOfBits(bits);
    m_bitcrusherR.setTargetSampleRate(rate);
    m_bitcrusherR.setJitter(jitter);
}

void CrusherModule::ProcessMono(float in) {
    if (!m_isEnabled) {
        m_audioLeft = in;
        m_audioRight = in;
        return;
    }

    float crushed = m_bitcrusherL.Process(in);
    float crunch = crushed - in;
    m_crunchBuf[m_crunchIndex] = crunch;
    if (m_crunchIndex >= static_cast<int>(resample_factor) - 1) {
        std::span<const float, resample_factor> in_chunk(m_crunchBuf, resample_factor);
        float decimated = m_crunchDecimator(in_chunk);
        if (m_cachedSub > 0.0f) {
            m_octaveGen.update(decimated);
            auto up = m_crunchInterpolator(m_octaveGen.down2());
            for (size_t j = 0; j < resample_factor; ++j) {
                m_crunchUp[j] = up[j];
            }
        } else {
            for (size_t j = 0; j < resample_factor; ++j) {
                m_crunchUp[j] = 0.0f;
            }
        }
        m_crunchIndex = 0;
    } else {
        m_crunchIndex++;
    }

    float sub_sample = m_crunchUp[m_crunchIndex];
    float wet = crushed + m_cachedSub * sub_sample;
    m_audioLeft = m_lpFilter[0](wet) * m_cachedLevel;
    m_audioRight = m_audioLeft;
}

void CrusherModule::ProcessStereo(float inL, float inR) {
    if (!m_isEnabled) {
        m_audioLeft = inL;
        m_audioRight = inR;
        return;
    }

    float outL = m_bitcrusherL.Process(inL);
    float outR = m_bitcrusherR.Process(inR);

    float crunchMono = 0.5f * ((outL - inL) + (outR - inR));
    m_crunchBuf[m_crunchIndex] = crunchMono;
    if (m_crunchIndex >= static_cast<int>(resample_factor) - 1) {
        std::span<const float, resample_factor> in_chunk(m_crunchBuf, resample_factor);
        float decimated = m_crunchDecimator(in_chunk);
        if (m_cachedSub > 0.0f) {
            m_octaveGen.update(decimated);
            auto up = m_crunchInterpolator(m_octaveGen.down1());
            for (size_t j = 0; j < resample_factor; ++j) {
                m_crunchUp[j] = up[j];
            }
        } else {
            for (size_t j = 0; j < resample_factor; ++j) {
                m_crunchUp[j] = 0.0f;
            }
        }
        m_crunchIndex = 0;
    } else {
        m_crunchIndex++;
    }

    float sub_sample = m_crunchUp[m_crunchIndex];
    float wetL = outL + m_cachedSub * sub_sample;
    float wetR = outR + m_cachedSub * sub_sample;

    m_audioLeft = (m_cachedDryGain * inL + m_cachedWetGain * m_lpFilter[0](wetL)) * m_cachedLevel;
    m_audioRight = (m_cachedDryGain * inR + m_cachedWetGain * m_lpFilter[1](wetR)) * m_cachedLevel;
}
