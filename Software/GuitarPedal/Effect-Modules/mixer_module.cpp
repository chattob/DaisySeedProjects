#include "mixer_module.h"

using namespace bkshepherd;

const ParameterMetaData MixerModule::s_metaData[s_paramCount] = {
    {
        name: "Ch 1 (input)",
        valueType: ParameterValueType::Float,
        valueCurve: ParameterValueCurve::Linear,
        valueBinCount: 0,
        valueBinNames: nullptr,
        defaultValue: {.float_value = 1.0f},
        knobMapping: -1,
        midiCCMapping: -1,
        minValue: 0,
        maxValue: 1,
        fineStepSize: 0.01f
    },
    {
        name: "Ch 2",
        valueType: ParameterValueType::Float,
        valueCurve: ParameterValueCurve::Linear,
        valueBinCount: 0,
        valueBinNames: nullptr,
        defaultValue: {.float_value = 0.0f},
        knobMapping: -1,
        midiCCMapping: -1,
        minValue: 0,
        maxValue: 1,
        fineStepSize: 0.01f
    },
    {
        name: "Ch 3",
        valueType: ParameterValueType::Float,
        valueCurve: ParameterValueCurve::Linear,
        valueBinCount: 0,
        valueBinNames: nullptr,
        defaultValue: {.float_value = 0.0f},
        knobMapping: -1,
        midiCCMapping: -1,
        minValue: 0,
        maxValue: 1,
        fineStepSize: 0.01f
    },
    {
        name: "Ch 4",
        valueType: ParameterValueType::Float,
        valueCurve: ParameterValueCurve::Linear,
        valueBinCount: 0,
        valueBinNames: nullptr,
        defaultValue: {.float_value = 0.0f},
        knobMapping: -1,
        midiCCMapping: -1,
        minValue: 0,
        maxValue: 1,
        fineStepSize: 0.01f
    },
    {
        name: "Master",
        valueType: ParameterValueType::Float,
        valueCurve: ParameterValueCurve::Linear,
        valueBinCount: 0,
        valueBinNames: nullptr,
        defaultValue: {.float_value = 1.0f},
        knobMapping: -1,
        midiCCMapping: -1,
        minValue: 0,
        maxValue: 1,
        fineStepSize: 0.01f
    },
    {
        name: "Pan",
        valueType: ParameterValueType::Float,
        valueCurve: ParameterValueCurve::Linear,
        valueBinCount: 0,
        valueBinNames: nullptr,
        defaultValue: {.float_value = 0.5f},
        knobMapping: -1,
        midiCCMapping: -1,
        minValue: 0,
        maxValue: 1,
        fineStepSize: 0.01f
    },
};

MixerModule::MixerModule() : BaseEffectModule() {
    m_name = "Mixer";
    m_paramMetaData = s_metaData;
    InitParams(s_paramCount);

    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        m_channelCaptured[ch] = false;
    }
}

void MixerModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
}

void MixerModule::ResetCaptures() {
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        m_channelCaptured[ch] = false;
    }
}

void MixerModule::CaptureChannel(int channel, AudioHandle::InputBuffer in, size_t size) {
    if (channel < 0 || channel >= NUM_CHANNELS) return;

    size_t copySize = (size < MAX_BLOCK_SIZE) ? size : MAX_BLOCK_SIZE;
    for (size_t i = 0; i < copySize; i++) {
        m_bufferL[channel][i] = in[0][i];
        m_bufferR[channel][i] = in[1][i];
    }
    m_channelCaptured[channel] = true;
}

void MixerModule::BlockPreProcessing(size_t size) {
    m_blockSize = (size < MAX_BLOCK_SIZE) ? size : MAX_BLOCK_SIZE;
    m_sampleIndex = 0;

    m_levels[0] = GetParameterAsFloat(CH1_LEVEL);
    m_levels[1] = GetParameterAsFloat(CH2_LEVEL);
    m_levels[2] = GetParameterAsFloat(CH3_LEVEL);
    m_levels[3] = GetParameterAsFloat(CH4_LEVEL);
    m_master = GetParameterAsFloat(MASTER_LEVEL);

    float pan = GetParameterAsFloat(PAN);
    if (pan < 0.0f) {
        pan = 0.0f;
    } else if (pan > 1.0f) {
        pan = 1.0f;
    }
    m_panL = (pan <= 0.5f) ? 1.0f : 2.0f * (1.0f - pan);
    m_panR = (pan >= 0.5f) ? 1.0f : 2.0f * pan;
}

void MixerModule::ProcessStereo(float inL, float inR) {
    float sumL = inL * m_levels[0];
    float sumR = inR * m_levels[0];
    const size_t sampleIndex = m_sampleIndex;

    for (int ch = 1; ch < NUM_CHANNELS; ch++) {
        if (m_channelCaptured[ch] && sampleIndex < m_blockSize) {
            sumL += m_bufferL[ch][sampleIndex] * m_levels[ch];
            sumR += m_bufferR[ch][sampleIndex] * m_levels[ch];
        }
    }

    m_audioLeft = sumL * m_master * m_panL;
    m_audioRight = sumR * m_master * m_panR;

    if (m_sampleIndex < m_blockSize) {
        ++m_sampleIndex;
    }
}
