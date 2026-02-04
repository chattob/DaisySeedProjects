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

void MixerModule::ProcessStereoBlock(AudioHandle::InputBuffer in,
                                     AudioHandle::OutputBuffer out,
                                     size_t size) {
    // Auto-capture input as channel 0 (wet signal from previous effects)
    CaptureChannel(0, in, size);

    float levels[NUM_CHANNELS] = {
        GetParameterAsFloat(CH1_LEVEL),
        GetParameterAsFloat(CH2_LEVEL),
        GetParameterAsFloat(CH3_LEVEL),
        GetParameterAsFloat(CH4_LEVEL)
    };
    float master = GetParameterAsFloat(MASTER_LEVEL);

    for (size_t i = 0; i < size; i++) {
        float sumL = 0.0f;
        float sumR = 0.0f;

        for (int ch = 0; ch < NUM_CHANNELS; ch++) {
            if (m_channelCaptured[ch]) {
                sumL += m_bufferL[ch][i] * levels[ch];
                sumR += m_bufferR[ch][i] * levels[ch];
            }
        }

        out[0][i] = sumL * master;
        out[1][i] = sumR * master;
    }
}
