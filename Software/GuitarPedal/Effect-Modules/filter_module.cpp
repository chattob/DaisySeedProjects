#include "filter_module.h"

using namespace bkshepherd;

static const int s_paramCount = 1;
static const ParameterMetaData s_metaData[s_paramCount] = {{
                                                               name : "Cutoff frequency",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 0.0f},
                                                               knobMapping : 0,
                                                               midiCCMapping : -1
                                                           }
                                                        };

// Default Constructor
FilterModule::FilterModule() : BaseEffectModule() {
    // Set the name of the effect
    m_name = "Filter";

    // Setup the meta data reference for this Effect
    m_paramMetaData = s_metaData;

    // Initialize Parameters for this Effect
    this->InitParams(s_paramCount);
}

// Destructor
FilterModule::~FilterModule() {
    // No Code Needed
}

void FilterModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);

    filter_range_ = 3000.0f;
    
    sv_filter_.Init(sample_rate);
    sv_filter_.SetRes(0.35f);
}

void FilterModule::ProcessStereo(float inL, float inR) {
    BaseEffectModule::ProcessStereo(inL, inR);

    float cutoff_freq = (GetParameterAsFloat(0) - 0.5f) * 2.0f * filter_range_;
    if (cutoff_freq < 0.0f) {
        cutoff_freq = 1.5 * (-filter_range_ - cutoff_freq);
    }

    sv_filter_.SetFreq(std::abs(cutoff_freq));
    sv_filter_.Process(inL);

    float out;
    if (cutoff_freq > 0.0f) {
        out = sv_filter_.High();
    } else if (cutoff_freq < 0.0f) {
        out = sv_filter_.Low();
    }

    m_audioLeft = out;
    m_audioRight = out;
}

float FilterModule::GetBrightnessForLED(int led_id) const {
    if (led_id == 0) {
        return 1.0;
    } else {
        return 0.0;
    }
}
