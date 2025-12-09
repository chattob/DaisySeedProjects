#include "mackity_module.h"

using namespace bkshepherd;

static const int s_paramCount = 3;
static const ParameterMetaData s_metaData[s_paramCount] = {{
                                                               name : "In Trim",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 0.5f},
                                                               knobMapping : 0,
                                                               midiCCMapping : -1
                                                           },
                                                           {
                                                               name : "Out Pad",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 0.5f},
                                                               knobMapping : 1,
                                                               midiCCMapping : -1
                                                           },
                                                           {
                                                               name : "Dry/wet",
                                                               valueType : ParameterValueType::Float,
                                                               valueBinCount : 0,
                                                               defaultValue : {.float_value = 0.5f},
                                                               knobMapping : 2,
                                                               midiCCMapping : -1
                                                           }
                                                        };

// Default Constructor
MackityModule::MackityModule() : BaseEffectModule() {
    // Set the name of the effect
    m_name = "Mackity";

    // Setup the meta data reference for this Effect
    m_paramMetaData = s_metaData;

    // Initialize Parameters for this Effect
    this->InitParams(s_paramCount);
}

// Destructor
MackityModule::~MackityModule() {
    // No Code Needed
}

void MackityModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);
    sample_rate_ = sample_rate;
}
void MackityModule::ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    if (m_isEnabled) {
        const float* inL  = in[0];
        const float* inR  = in[1];
        float*       outL = out[0];
        float*       outR = out[1];

        float mix = GetParameterAsFloat(2);

        // === parameter & coefficient setup (per block, like Airwindows per process call) ===
        double overallscale = 1.0;
        overallscale /= 44100.0;
        overallscale *= static_cast<double>(sample_rate_);
        double inTrim = static_cast<double>(GetParameterAsFloat(0)) * 10.0;
        double outPad = static_cast<double>(GetParameterAsFloat(1));
        inTrim *= inTrim; // squared
        double iirAmountA = 0.001860867 / overallscale;
        double iirAmountB = 0.000287496 / overallscale;

        // biquad frequency is normalized by sample rate
        biquadB_[0] = biquadA_[0] = 19160.0 / static_cast<double>(sample_rate_);
        biquadA_[1] = 0.431684981684982;
        biquadB_[1] = 1.1582298;

        // biquadA (lowpass)
        double K    = std::tan(M_PI * biquadA_[0]);
        double norm = 1.0 / (1.0 + K / biquadA_[1] + K * K);
        biquadA_[2] = K * K * norm;
        biquadA_[3] = 2.0 * biquadA_[2];
        biquadA_[4] = biquadA_[2];
        biquadA_[5] = 2.0 * (K * K - 1.0) * norm;
        biquadA_[6] = (1.0 - K / biquadA_[1] + K * K) * norm;

        // biquadB (lowpass)
        K    = std::tan(M_PI * biquadB_[0]);
        norm = 1.0 / (1.0 + K / biquadB_[1] + K * K);
        biquadB_[2] = K * K * norm;
        biquadB_[3] = 2.0 * biquadB_[2];
        biquadB_[4] = biquadB_[2];
        biquadB_[5] = 2.0 * (K * K - 1.0) * norm;
        biquadB_[6] = (1.0 - K / biquadB_[1] + K * K) * norm;

        // === sample loop ===
        for (size_t i = 0; i < size; ++i){
            double inputSampleL = static_cast<double>(inL[i]);
            double inputSampleR = static_cast<double>(inR[i]);

            // tiny denormal noise (kept for fidelity, but you could remove)
            if (std::fabs(inputSampleL) < 1.18e-23)
                inputSampleL = fpdL_ * 1.18e-17;
            if (std::fabs(inputSampleR) < 1.18e-23)
                inputSampleR = fpdR_ * 1.18e-17;

            // pre-emphasis IIR HP (A)
            if (std::fabs(iirSampleAL_) < 1.18e-37)
                iirSampleAL_ = 0.0;
            iirSampleAL_ = (iirSampleAL_ * (1.0 - iirAmountA)) + (inputSampleL * iirAmountA);
            inputSampleL -= iirSampleAL_;
            if (std::fabs(iirSampleAR_) < 1.18e-37)
                iirSampleAR_ = 0.0;
            iirSampleAR_ = (iirSampleAR_ * (1.0 - iirAmountA)) + (inputSampleR * iirAmountA);
            inputSampleR -= iirSampleAR_;

            // input gain
            if (inTrim != 1.0){
                inputSampleL *= inTrim;
                inputSampleR *= inTrim;
            }

            // biquadA (DF1) left
            double outSampleL = biquadA_[2] * inputSampleL
                            + biquadA_[3] * biquadA_[7]
                            + biquadA_[4] * biquadA_[8]
                            - biquadA_[5] * biquadA_[9]
                            - biquadA_[6] * biquadA_[10];
            biquadA_[8]  = biquadA_[7];
            biquadA_[7]  = inputSampleL;
            inputSampleL = outSampleL;
            biquadA_[10] = biquadA_[9];
            biquadA_[9]  = inputSampleL;

            // biquadA (DF1) right
            double outSampleR = biquadA_[2] * inputSampleR
                            + biquadA_[3] * biquadA_[11]
                            + biquadA_[4] * biquadA_[12]
                            - biquadA_[5] * biquadA_[13]
                            - biquadA_[6] * biquadA_[14];
            biquadA_[12] = biquadA_[11];
            biquadA_[11] = inputSampleR;
            inputSampleR = outSampleR;
            biquadA_[14] = biquadA_[13];
            biquadA_[13] = inputSampleR;

            // waveshaper
            if (inputSampleL > 1.0)
                inputSampleL = 1.0;
            if (inputSampleL < -1.0)
                inputSampleL = -1.0;
            inputSampleL -= std::pow(inputSampleL, 5) * 0.1768;
            if (inputSampleR > 1.0)
                inputSampleR = 1.0;
            if (inputSampleR < -1.0)
                inputSampleR = -1.0;
            inputSampleR -= std::pow(inputSampleR, 5) * 0.1768;

            // biquadB (DF1) left
            outSampleL = biquadB_[2] * inputSampleL
                    + biquadB_[3] * biquadB_[7]
                    + biquadB_[4] * biquadB_[8]
                    - biquadB_[5] * biquadB_[9]
                    - biquadB_[6] * biquadB_[10];
            biquadB_[8]  = biquadB_[7];
            biquadB_[7]  = inputSampleL;
            inputSampleL = outSampleL;
            biquadB_[10] = biquadB_[9];
            biquadB_[9]  = inputSampleL;

            // biquadB (DF1) right
            outSampleR = biquadB_[2] * inputSampleR
                    + biquadB_[3] * biquadB_[11]
                    + biquadB_[4] * biquadB_[12]
                    - biquadB_[5] * biquadB_[13]
                    - biquadB_[6] * biquadB_[14];
            biquadB_[12] = biquadB_[11];
            biquadB_[11] = inputSampleR;
            inputSampleR = outSampleR;
            biquadB_[14] = biquadB_[13];
            biquadB_[13] = inputSampleR;

            // post-emphasis IIR HP (B)
            if (std::fabs(iirSampleBL_) < 1.18e-37)
                iirSampleBL_ = 0.0;
            iirSampleBL_ = (iirSampleBL_ * (1.0 - iirAmountB)) + (inputSampleL * iirAmountB);
            inputSampleL -= iirSampleBL_;
            if (std::fabs(iirSampleBR_) < 1.18e-37)
                iirSampleBR_ = 0.0;
            iirSampleBR_ = (iirSampleBR_ * (1.0 - iirAmountB)) + (inputSampleR * iirAmountB);
            inputSampleR -= iirSampleBR_;

            // output pad
            if (outPad != 1.0)
            {
                inputSampleL *= outPad;
                inputSampleR *= outPad;
            }
            outL[i] = mix * static_cast<float>(inputSampleL) + (1.0 - mix) * inL[i];
            outR[i] = mix * static_cast<float>(inputSampleR) + (1.0 - mix) * inR[i];
        }
    } else {
        for (size_t i = 0; i < size; i++) {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
    }
}
