#include "phase_vocoder_module.h"
#include "clouds/dsp/frame.h"

using namespace bkshepherd;

static const int s_paramCount = 2;
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
                                                           };

// Default Constructor
PhaseVocoderModule::PhaseVocoderModule() : BaseEffectModule() {
    // Set the name of the effect
    m_name = "Phase vocoder";

    // Setup the meta data reference for this Effect
    m_paramMetaData = s_metaData;

    // Initialize Parameters for this Effect
    this->InitParams(s_paramCount);
}

// Destructor
PhaseVocoderModule::~PhaseVocoderModule() {
    // No Code Needed
}

void PhaseVocoderModule::ProcessStereo(float inL, float inR) {
    BaseEffectModule::ProcessStereo(inL, inR);

    if (m_isEnabled) {
        parameters_.spectral.quantization = parameters_.texture;
        parameters_.spectral.refresh_rate = 0.01f + 0.99f * parameters_.density;
        float warp = parameters_.size - 0.5f;
        parameters_.spectral.warp = 4.0f * warp * warp * warp + 0.5f;
        
        float randomization = parameters_.density - 0.5f;
        randomization *= randomization * 4.2f;
        randomization -= 0.05f;
        CONSTRAIN(randomization, 0.0f, 1.0f);
        parameters_.spectral.phase_randomization = randomization;
        /*phase_vocoder_.Process(parameters_, input, output, size);

        m_audioLeft = output.l;
        m_audioRight = ouput.r;*/
    }
}