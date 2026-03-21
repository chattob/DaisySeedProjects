#include "delay_module.h"
#include "../Util/audio_utilities.h"

using namespace bkshepherd;

static const char *s_waveBinNames[6] = {"Sine", "Triangle", "Saw", "Ramp",
                                        "Square", "Tape"}; //, "Poly Tri", "Poly Saw", "Poly Sqr"};  // Horrible loud sound when switching to
                                                   // poly tri, not every time, TODO whats going on? (I suspect electro smith broke
                                                   // the poly tri osc, the same happens in the tremolo too)
static const char *s_modParamNames[4] = {"None", "DelayTime", "DelayLevel", "DelayPan"};
static const char *s_delayModes[3] = {"Normal", "Triplett", "Dotted 8th"};
static const char *s_delayTypes[6] = {"Forward", "Reverse", "Octave", "ReverseOct", "Dual", "DualOct"};
static_assert((sizeof(s_delayTypes) / sizeof(s_delayTypes[0])) == DelayModule::DELAY_TYPE_COUNT,
              "Delay type labels must match DelayType enum count");

DelayLineRevOct<float, MAX_DELAY_NORM> DSY_SDRAM_BSS delayLineLeft;
DelayLineRevOct<float, MAX_DELAY_NORM> DSY_SDRAM_BSS delayLineRight;
DelayLineReverse<float, MAX_DELAY_REV> DSY_SDRAM_BSS delayLineRevLeft;
DelayLineReverse<float, MAX_DELAY_REV> DSY_SDRAM_BSS delayLineRevRight;
DelayLine<float, MAX_DELAY_SPREAD> DSY_SDRAM_BSS delayLineSpread;

static constexpr int s_paramCount = DelayModule::PARAM_COUNT; // TODO: TEST STARTING WITH THE EXTREMES OF ALL PARAMETERS (high and low, this is where errors tend to occur)
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Delay Time",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : -1,
        midiCCMapping : 14
    }, // mod
    {
        name : "D Feedback",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : -1,
        midiCCMapping : 15
    },
    {
        name : "Delay Mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : -1,
        midiCCMapping : 16
    },

    {
        name : "Delay Mode",
        valueType : ParameterValueType::Binned,
        valueBinCount : 3,
        valueBinNames : s_delayModes,
        defaultValue : {.uint_value = 0},
        knobMapping : -1,
        midiCCMapping : 17
    },
    {
        name : "Delay Type",
        valueType : ParameterValueType::Binned,
        valueBinCount : DelayModule::DELAY_TYPE_COUNT,
        valueBinNames : s_delayTypes,
        defaultValue : {.uint_value = 0},
        knobMapping : -1,
        midiCCMapping : 18
    },

    {
        name : "Delay LPF",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : -1,
        midiCCMapping : 19
    }, // mod
    {
        name : "D Spread",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : -1,
        midiCCMapping : 20
    },

    {
        name : "Mod Amplitude",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : -1,
        midiCCMapping : 21
    },
    {
        name : "Mod Freq",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : -1,
        midiCCMapping : 22
    },
    {
        name : "Mod Param",
        valueType : ParameterValueType::Binned,
        valueBinCount : 4,
        valueBinNames : s_modParamNames,
        defaultValue : {.uint_value = 0},
        knobMapping : -1,
        midiCCMapping : 23
    },
    {
        name : "Mod Wave",
        valueType : ParameterValueType::Binned,
        valueBinCount : 6,
        valueBinNames : s_waveBinNames,
        defaultValue : {.uint_value = 0},
        knobMapping : -1,
        midiCCMapping : 24
    },
    {
        name : "Sync Mod F",
        valueType : ParameterValueType::Bool,
        defaultValue : {.uint_value = 0},
        knobMapping : -1,
        midiCCMapping : 25
    }};

// Default Constructor
DelayModule::DelayModule()
    : BaseEffectModule(), m_delaylpFreqMin(300.0f), m_delaylpFreqMax(20000.0f), m_delaySamplesMin(2400.0f),
      m_delaySamplesMax(192000.0f), m_delaySpreadMin(24.0f), m_delaySpreadMax(2400.0f), m_pdelRight_out(0.0),
      m_currentModLeft(1.0f), m_currentModRight(1.0f),
      m_modOscFreqMin(0.0), m_modOscFreqMax(3.0), m_LEDValue(1.0f) {
    // Set the name of the effect
    m_name = "Delay";

    // Setup the meta data reference for this Effect
    m_paramMetaData = s_metaData;

    // Initialize Parameters for this Effect
    this->InitParams(s_paramCount);
}

// Destructor
DelayModule::~DelayModule() {
    // No Code Needed
}

void DelayModule::UpdateLEDRate() {
    // Update the LED oscillator frequency based on the current timeParam
    float timeParam = GetParameterAsFloat(0);
    float delaySamples = m_delaySamplesMin + (m_delaySamplesMax - m_delaySamplesMin) * timeParam;
    float delayFreq = effect_samplerate / delaySamples;
    led_osc.SetFreq(delayFreq / 2.0);
}

void DelayModule::Init(float sample_rate) {
    BaseEffectModule::Init(sample_rate);

    delayLineLeft.Init();
    delayLineRevLeft.Init();
    delayLeft.del = &delayLineLeft;
    delayLeft.delreverse = &delayLineRevLeft;
    delayLeft.delayTarget = 24000; // in samples
    delayLeft.currentDelay = delayLeft.delayTarget;
    delayLeft.feedback = 0.0;
    delayLeft.active = true; // Default to no delay
    delayLeft.toneOctLP.Init(sample_rate);
    delayLeft.toneOctLP.SetFreq(20000.0);

    delayLineRight.Init();
    delayLineRevRight.Init();
    delayRight.del = &delayLineRight;
    delayRight.delreverse = &delayLineRevRight;
    delayRight.delayTarget = 24000; // in samples
    delayRight.currentDelay = delayRight.delayTarget;
    delayRight.feedback = 0.0;
    delayRight.active = true; // Default to no
    delayRight.toneOctLP.Init(sample_rate);
    delayRight.toneOctLP.SetFreq(20000.0);

    delayLineSpread.Init();
    delaySpread.del = &delayLineSpread;
    delaySpread.delayTarget = 1500; // in samples
    delaySpread.currentDelay = delaySpread.delayTarget;
    delaySpread.active = true;

    effect_samplerate = sample_rate;

    led_osc.Init(sample_rate);
    led_osc.SetWaveform(1);
    led_osc.SetFreq(2.0);

    modOsc.Init(sample_rate);
    modOsc.SetAmp(1.0);

    modTapeLeft.Init(sample_rate);
    modTapeRight.Init(sample_rate);

    auto gains = EnergyCrossfade(GetParameterAsFloat(2));
    delayWetMix = gains.wet;
    delayDryMix = gains.dry;
}

void DelayModule::SetEnabled(bool isEnabled) {
    BaseEffectModule::SetEnabled(isEnabled);

    if (!isEnabled) {
        if (delayLeft.del != nullptr) {
            delayLeft.del->Reset();
        }
        if (delayRight.del != nullptr) {
            delayRight.del->Reset();
        }
        if (delayLeft.delreverse != nullptr) {
            delayLeft.delreverse->Reset();
        }
        if (delayRight.delreverse != nullptr) {
            delayRight.delreverse->Reset();
        }
        if (delaySpread.del != nullptr) {
            delaySpread.del->Reset();
        }

        // Re-apply delay mode tap settings, since Reset() clears second-tap fractions.
        if (delayLeft.del != nullptr && delayRight.del != nullptr) {
            ParameterChanged(DELAY_MODE);
        }
    }
}

void DelayModule::ParameterChanged(int parameter_id) {
    if (parameter_id == 0) { // Delay Time
        UpdateLEDRate();
    } else if (parameter_id == 2) { // Delay Mix
        auto gains = EnergyCrossfade(GetParameterAsFloat(2));
        delayWetMix = gains.wet;
        delayDryMix = gains.dry;
    } else if (parameter_id == 3) { // Delay Mode
        int delay_mode_temp = (GetParameterAsBinnedValue(3) - 1);
        if (delay_mode_temp > 0) {
            delayLeft.secondTapOn = true;  // triplett, dotted 8th
            delayRight.secondTapOn = true; // triplett, dotted 8th
            if (delay_mode_temp == 1) {
                delayLeft.del->set2ndTapFraction(0.6666667);  // triplett
                delayRight.del->set2ndTapFraction(0.6666667); // triplett
            } else if (delay_mode_temp == 2) {
                delayLeft.del->set2ndTapFraction(0.75);  // dotted eighth
                delayRight.del->set2ndTapFraction(0.75); // dotted eighth
            }
        } else {
            delayLeft.secondTapOn = false;
            delayRight.secondTapOn = false;
        }
    } else if (parameter_id == 5) {
        delayLeft.toneOctLP.SetFreq(m_delaylpFreqMin + (m_delaylpFreqMax - m_delaylpFreqMin) * GetParameterAsFloat(5));
        delayRight.toneOctLP.SetFreq(m_delaylpFreqMin + (m_delaylpFreqMax - m_delaylpFreqMin) * GetParameterAsFloat(5));
    }
}

void DelayModule::ProcessModulation(size_t size) {
    int modParam = (GetParameterAsBinnedValue(9) - 1);
    // Calculate Modulation
    int waveForm = GetParameterAsBinnedValue(10) - 1;
    float wowDepth = 2.0f;
    float flutterDepth = 2.0f;

    if (waveForm == 5) {
        float freq = GetParameterAsFloat(8) * size;
        float wowRate = 0.2f + 2.0f * freq;
        float flutterRate = 2.0f + 5.0f * freq;
        m_currentModLeft = modTapeLeft.GetTapeSpeed(wowRate, flutterRate, wowDepth, flutterDepth);
        m_currentModRight = modTapeRight.GetTapeSpeed(wowRate, flutterRate, wowDepth, flutterDepth);
    } else {
        modOsc.SetWaveform(waveForm);

        if (GetParameterAsBool(11)) { // If mod frequency synced to delay time, override mod rate setting
            float dividor;
            if (modParam == 2 || modParam == 3) {
                dividor = 2.0;
            } else {
                dividor = 4.0;
            }
            float freq = (effect_samplerate / delayLeft.delayTarget) * size / dividor;
            modOsc.SetFreq(freq);
        } else {
            modOsc.SetFreq(m_modOscFreqMin + (m_modOscFreqMax - m_modOscFreqMin) * GetParameterAsFloat(8));
        }

        // Ease the effect value into it's target to avoid clipping with square or sawtooth waves
        fonepole(m_currentModLeft, modOsc.Process(), .01f);
        m_currentModRight = m_currentModLeft;
    }

    float modLeft = m_currentModLeft;
    float modRight = m_currentModRight;
    float mod_amount = GetParameterAsFloat(7);

    // {"None", "DelayTime", "DelayLevel", "Level", "DelayPan"};
    if (modParam == 1) {
        float timeParam = GetParameterAsFloat(DELAY_TIME);
        const float D_min = 1.0f; // minimum allowable delay time: 1 sample
        const float depth = 500.0f;
        float delayTargetLeft;
        float delayTargetRight;

        if (waveForm == 5) {
            // Tape flutter mode with dynamic min
            const float M     = wowDepth + 0.2f * flutterDepth; // Max amplitude of tape modulation.
            float baseMin = D_min + M * mod_amount * depth;
            float baseMax = m_delaySamplesMax; // or some flutter-specific max

            float base = baseMin + (baseMax - baseMin) * timeParam;

            delayTargetLeft = base + modLeft * mod_amount * depth;
            delayTargetRight = base + modRight * mod_amount * depth;
        } else {
            float base = m_delaySamplesMin + (m_delaySamplesMax - m_delaySamplesMin) * timeParam;
            delayTargetLeft = base + modLeft * mod_amount * depth;
            delayTargetRight = base + modRight * mod_amount * depth;
        }

        if (delayTargetLeft < D_min) {
            delayTargetLeft = D_min;
        }
        if (delayTargetLeft > MAX_DELAY_NORM - 2) {
            delayTargetLeft = MAX_DELAY_NORM - 2;
        }

        if (delayTargetRight < D_min) {
            delayTargetRight = D_min;
        }
        if (delayTargetRight > MAX_DELAY_NORM - 2) {
            delayTargetRight = MAX_DELAY_NORM - 2;
        }

        delayLeft.delayTarget = delayTargetLeft;
        delayRight.delayTarget = delayTargetRight;
    } else if (modParam == 2) {
        float mod_level_left = modLeft * mod_amount + (1.0f - mod_amount);
        float mod_level_right = modRight * mod_amount + (1.0f - mod_amount);
        delayLeft.level = mod_level_left;
        delayRight.level = mod_level_right;
        delayLeft.level_reverse = mod_level_left;
        delayRight.level_reverse = mod_level_right;

    } else if (modParam == 3) {
        float mod = 0.5f * (modLeft + modRight);
        _level = mod * mod_amount + (1.0f - mod_amount);

    } else if (modParam == 4) {
        float mod_left = modLeft * mod_amount + (1.0f - mod_amount);
        float mod_right = modRight * mod_amount + (1.0f - mod_amount);
        delayLeft.level = mod_left;
        delayRight.level = 1.0f - mod_right;
        delayLeft.level_reverse = mod_left;
        delayRight.level_reverse = 1.0f - mod_right;
    }
}

void DelayModule::BlockPreProcessing(size_t size) {
    if (!m_isEnabled) {
        return;
    }

    m_LEDValue = led_osc.Process(); // update the tempo LED
    m_cachedDelayType = GetParameterAsBinnedValue(DELAY_TYPE);

    float timeParam = GetParameterAsFloat(DELAY_TIME);
    delayLeft.delayTarget = m_delaySamplesMin + (m_delaySamplesMax - m_delaySamplesMin) * timeParam;
    delayRight.delayTarget = m_delaySamplesMin + (m_delaySamplesMax - m_delaySamplesMin) * timeParam;

    delayLeft.feedback = GetParameterAsFloat(D_FEEDBACK);
    delayRight.feedback = GetParameterAsFloat(D_FEEDBACK);

    bool reverseMode = (m_cachedDelayType == DELAY_TYPE_REVERSE || m_cachedDelayType == DELAY_TYPE_REVERSE_OCT);
    delayLeft.reverseMode = reverseMode;
    delayRight.reverseMode = reverseMode;

    bool octaveMode = (m_cachedDelayType == DELAY_TYPE_OCTAVE || m_cachedDelayType == DELAY_TYPE_REVERSE_OCT
                       || m_cachedDelayType == DELAY_TYPE_DUAL_OCT);
    delayLeft.del->setOctave(octaveMode);
    delayRight.del->setOctave(octaveMode);

    bool dualDelay = (m_cachedDelayType == DELAY_TYPE_DUAL || m_cachedDelayType == DELAY_TYPE_DUAL_OCT);
    delayLeft.dual_delay = dualDelay;
    delayRight.dual_delay = dualDelay;

    float spread = GetParameterAsFloat(D_SPREAD);
    if (dualDelay) {
        delayLeft.level = spread + 1.0f;
        delayRight.level = 1.0f - spread;
        delayLeft.level_reverse = 1.0f - spread;
        delayRight.level_reverse = spread + 1.0f;
    } else {
        delayLeft.level = 1.0f;
        delayRight.level = 1.0f;
        delayLeft.level_reverse = 1.0f;
        delayRight.level_reverse = 1.0f;
    }

    delaySpread.delayTarget = m_delaySpreadMin + (m_delaySpreadMax - m_delaySpreadMin) * spread;
    m_cachedApplySpread = (GetParameterRaw(D_SPREAD) > 0)
                          && m_cachedDelayType != DELAY_TYPE_REVERSE_OCT
                          && m_cachedDelayType != DELAY_TYPE_DUAL;

    // Modulation, this overwrites any previous parameter settings for the modulated param - TODO Better way to do this for less
    // processing?
    ProcessModulation(size);
}

void DelayModule::ProcessStereo(float inL, float inR) {
    if (!m_isEnabled) {
        m_audioLeft = inL;
        m_audioRight = inR;
        return;
    }

    float delLeft_out = delayLeft.Process(inL);
    float delRight_out = delayRight.Process(inR);

    float delSpread_out = delaySpread.Process(delRight_out);
    if (m_cachedApplySpread) {
        delRight_out = delSpread_out;
    }

    m_audioLeft = delLeft_out * delayWetMix + inL * delayDryMix;
    m_audioRight = delRight_out * delayWetMix + inR * delayDryMix;
}

// Set the delay time from the tap tempo  TODO: Currently the tap tempo led isn't set to delay time on pedal boot up, how to do this?
void DelayModule::SetTempo(uint32_t bpm) {
    float freq = tempo_to_freq(bpm);
    float delay_in_samples = effect_samplerate / freq;

    if (delay_in_samples <= m_delaySamplesMin) {
        SetParameterAsMagnitude(0, 0.0f);
    } else if (delay_in_samples >= m_delaySamplesMax) {
        SetParameterAsMagnitude(0, 1.0f);
    } else {
        // Get the parameter as close as we can to target tempo
        float magnitude =
            static_cast<float>(delay_in_samples - m_delaySamplesMin) / static_cast<float>(m_delaySamplesMax - m_delaySamplesMin);
        SetParameterAsMagnitude(0, magnitude);
    }
    UpdateLEDRate();
}

float DelayModule::GetBrightnessForLED(int led_id) const {
    float value = BaseEffectModule::GetBrightnessForLED(led_id);

    float ledValue = 0.0;
    if (m_LEDValue > 0.45) {
        ledValue = 1.0;
    } else {
        ledValue = 0.0;
    }

    if (led_id == 1) {
        return value * ledValue;
    }

    return value;
}
