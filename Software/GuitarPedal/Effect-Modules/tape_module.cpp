#include "tape_module.h"
#include "../Util/audio_utilities.h"

using namespace bkshepherd;

namespace {

constexpr size_t kMaxDelaySamples = static_cast<size_t>(4800.0f); // 100ms @ 48kHz
constexpr float kMinDelaySamples = 2400.0f;                       // 50ms @ 48kHz
constexpr float kDepthSamples = 500.0f;
constexpr float kWowDepth = 2.0f;
constexpr float kFlutterDepth = 2.0f;
constexpr float kSmoothing = 0.0002f;
constexpr float kMinValidDelay = 1.0f;

DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS s_delayLineLeft;
DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS s_delayLineRight;

static constexpr int s_paramCount = TapeModule::PARAM_COUNT;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Depth",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : -1,
        midiCCMapping : -1
    },
    {
        name : "Rate",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : -1,
        midiCCMapping : -1
    },
    {
        name : "Mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : -1,
        midiCCMapping : -1
    },
};

} // namespace

TapeModule::TapeModule()
    : BaseEffectModule(), m_currentDelayLeft(kMinDelaySamples), m_currentDelayRight(kMinDelaySamples), m_wet(1.0f), m_dry(0.0f) {
    m_name = "Tape";
    m_paramMetaData = s_metaData;
    InitParams(s_paramCount);
}

TapeModule::~TapeModule() {
    // No code needed
}

void TapeModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);

    const float depth = GetParameterAsFloat(DEPTH);
    const float modulationHeadroom = (kWowDepth + 0.2f * kFlutterDepth) * depth * kDepthSamples;
    float baseDelay = kMinDelaySamples + modulationHeadroom;
    const float maxDelay = static_cast<float>(kMaxDelaySamples) - 2.0f;
    if (baseDelay > maxDelay) {
        baseDelay = maxDelay;
    }
    m_currentDelayLeft = baseDelay;
    m_currentDelayRight = baseDelay;

    s_delayLineLeft.Init();
    s_delayLineRight.Init();
    s_delayLineLeft.SetDelay(m_currentDelayLeft);
    s_delayLineRight.SetDelay(m_currentDelayRight);

    m_modTapeLeft.Init(sample_rate);
    m_modTapeRight.Init(sample_rate);

    auto gains = EnergyCrossfade(GetParameterAsFloat(MIX));
    m_wet = gains.wet;
    m_dry = gains.dry;
}

void TapeModule::ParameterChanged(int parameter_id) {
    if (parameter_id == MIX) {
        auto gains = EnergyCrossfade(GetParameterAsFloat(MIX));
        m_wet = gains.wet;
        m_dry = gains.dry;
    }
}

void TapeModule::ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    if (!m_isEnabled) {
        for (size_t i = 0; i < size; i++) {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
        return;
    }

    const float depth = GetParameterAsFloat(DEPTH);
    const float rate = GetParameterAsFloat(RATE);

    const float wowRate = 0.2f + 2.0f * rate;
    const float flutterRate = 2.0f + 5.0f * rate;

    // Keep the base delay high enough so modulation does not cross 0 samples.
    const float modulationHeadroom = (kWowDepth + 0.2f * kFlutterDepth) * depth * kDepthSamples;
    const float baseDelay = kMinDelaySamples + modulationHeadroom;
    const float maxDelay = static_cast<float>(kMaxDelaySamples) - 2.0f;

    for (size_t i = 0; i < size; i++) {
        const float modLeft = m_modTapeLeft.GetTapeSpeed(wowRate, flutterRate, kWowDepth, kFlutterDepth);
        const float modRight = m_modTapeRight.GetTapeSpeed(wowRate, flutterRate, kWowDepth, kFlutterDepth);

        float targetDelayLeft = baseDelay + modLeft * depth * kDepthSamples;
        float targetDelayRight = baseDelay + modRight * depth * kDepthSamples;

        if (targetDelayLeft < kMinValidDelay) {
            targetDelayLeft = kMinValidDelay;
        } else if (targetDelayLeft > maxDelay) {
            targetDelayLeft = maxDelay;
        }

        if (targetDelayRight < kMinValidDelay) {
            targetDelayRight = kMinValidDelay;
        } else if (targetDelayRight > maxDelay) {
            targetDelayRight = maxDelay;
        }

        fonepole(m_currentDelayLeft, targetDelayLeft, kSmoothing);
        fonepole(m_currentDelayRight, targetDelayRight, kSmoothing);

        s_delayLineLeft.SetDelay(m_currentDelayLeft);
        s_delayLineRight.SetDelay(m_currentDelayRight);

        const float wetLeft = s_delayLineLeft.Read();
        const float wetRight = s_delayLineRight.Read();

        s_delayLineLeft.Write(in[0][i]);
        s_delayLineRight.Write(in[1][i]);

        out[0][i] = wetLeft * m_wet + in[0][i] * m_dry;
        out[1][i] = wetRight * m_wet + in[1][i] * m_dry;
    }
}
