#include "crusher_module.h"
#include "../Util/audio_utilities.h"

using namespace bkshepherd;

static const int s_paramCount = 6;
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
                                                           }};

// Default Constructor
CrusherModule::CrusherModule() : BaseEffectModule(), m_rateMin(100.0f), m_rateMax(48000.0f),
m_cutoffMin(500), m_cutoffMax(20000), lp_filter_(m_cutoffMax, 48000.0f) {

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
}

void CrusherModule::ProcessMonoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    float level = GetParameterAsFloat(LEVEL);
    float cutoff = m_cutoffMin + GetParameterAsFloat(CUTOFF) * (m_cutoffMax - m_cutoffMin);
    float bits = (float)GetParameterAsBinnedValue(BITS);
    float t = GetParameterAsFloat(RATE);     // 0..1
    float rate = m_rateMin * powf(m_rateMax / m_rateMin, t);
    float jitter = GetParameterAsFloat(JITTER);

    lp_filter_.config(cutoff, m_rateMax);

    m_bitcrusherL.setNumberOfBits(bits);
    m_bitcrusherL.setTargetSampleRate(rate);
    m_bitcrusherL.setJitter(jitter);

    for (size_t i = 0; i < size; i++) {
        out[0][i] = m_bitcrusherL.Process(in[0][i]) * level;
        out[1][i] = out[0][i];
    }
}

void CrusherModule::ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    float level = GetParameterAsFloat(LEVEL);
    float cutoff = m_cutoffMin + GetParameterAsFloat(CUTOFF) * (m_cutoffMax - m_cutoffMin);
    float bits = (float)GetParameterAsBinnedValue(BITS);
    float t = GetParameterAsFloat(RATE);     // 0..1
    float rate = m_rateMin * powf(m_rateMax / m_rateMin, t);
    float jitter = GetParameterAsFloat(JITTER);

    lp_filter_.config(cutoff, m_rateMax);

    m_bitcrusherL.setNumberOfBits(bits);
    m_bitcrusherL.setTargetSampleRate(rate);
    m_bitcrusherL.setJitter(jitter);
    m_bitcrusherR.setNumberOfBits(bits);
    m_bitcrusherR.setTargetSampleRate(rate);
    m_bitcrusherR.setJitter(jitter);

    auto gains = EnergyCrossfade(GetParameterAsFloat(MIX));

    for (size_t i = 0; i < size; i++) {
        float outL = m_bitcrusherL.Process(in[0][i]);
        float outR = m_bitcrusherR.Process(in[1][i]);

        out[0][i] = (gains.dry * in[0][i] + gains.wet * lp_filter_(outL)) * level;
        out[1][i] = (gains.dry * in[1][i] + gains.wet * lp_filter_(outR)) * level;
    }
}
