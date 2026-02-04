#pragma once
#ifndef MIXER_MODULE_H
#define MIXER_MODULE_H

#include <stdint.h>
#include "base_effect_module.h"

#ifdef __cplusplus

namespace bkshepherd {

class MixerModule : public BaseEffectModule {
public:
    static constexpr int NUM_CHANNELS = 4;
    static constexpr size_t MAX_BLOCK_SIZE = 256;

    MixerModule();
    ~MixerModule() override = default;

    void Init(float sample_rate) override;
    void ProcessStereoBlock(AudioHandle::InputBuffer in,
                           AudioHandle::OutputBuffer out,
                           size_t size) override;

    // Capture signal into a channel buffer (call at any point in chain)
    void CaptureChannel(int channel, AudioHandle::InputBuffer in, size_t size);

    // Reset capture flags (call at start of audio callback)
    void ResetCaptures();

    enum Param {
        CH1_LEVEL = 0,
        CH2_LEVEL,
        CH3_LEVEL,
        CH4_LEVEL,
        MASTER_LEVEL,
        PARAM_COUNT
    };

private:
    static const int s_paramCount = PARAM_COUNT;
    static const ParameterMetaData s_metaData[s_paramCount];

    float m_bufferL[NUM_CHANNELS][MAX_BLOCK_SIZE];
    float m_bufferR[NUM_CHANNELS][MAX_BLOCK_SIZE];
    bool m_channelCaptured[NUM_CHANNELS];
    size_t m_blockSize = 0;
};

} // namespace bkshepherd

#endif
#endif
