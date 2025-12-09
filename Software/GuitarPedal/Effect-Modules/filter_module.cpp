#include "filter_module.h"

using namespace bkshepherd;

static const int s_paramCount = 2;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Cutoff",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f}, // mid position
        knobMapping : 0,
        midiCCMapping : -1
    },
    {
        name : "HP Mode",
        valueType : ParameterValueType::Bool,   // false = LP, true = HP
        valueBinCount : 0,
        defaultValue : {.uint_value = 0},       // default: LPF
        knobMapping : 1,
        midiCCMapping : -1
    },
};

// Default Constructor
FilterModule::FilterModule()
: BaseEffectModule()
, cutoff_norm_(0.5f)
, hp_mode_(false)
, cutoff_min_(60.0f)      // adjust to taste
, cutoff_max_(8000.0f)    // adjust to taste
, hp_filter_(cutoff_min_, 48000.0f)  // dummy init, will be reconfigured
, lp_filter_(cutoff_max_, 48000.0f)
{
    m_name          = "Filter";
    m_paramMetaData = s_metaData;
    InitParams(s_paramCount);
}

// Destructor
FilterModule::~FilterModule() {}

void FilterModule::Init(float sample_rate)
{
    BaseEffectModule::Init(sample_rate);

    // Read initial parameter values
    cutoff_norm_ = GetParameterAsFloat(CUTOFF);
    hp_mode_     = GetParameterAsBool(HP_MODE);

    UpdateFilters();
}

void FilterModule::UpdateFilters()
{
    // Map normalized cutoff (0..1) to [cutoff_min_, cutoff_max_] using exponential mapping.
    const float norm = cutoff_norm_;

    const float min_hz = cutoff_min_;
    const float max_hz = cutoff_max_;

    // Avoid log(0); clamp
    const float n = (norm <= 0.0f) ? 0.0001f : (norm >= 1.0f ? 0.9999f : norm);

    const float cutoff =
        min_hz * std::pow(max_hz / min_hz, n); // exponential frequency mapping

    const float sr = GetSampleRate();

    hp_filter_.config(cutoff, sr);
    lp_filter_.config(cutoff, sr);
}

void FilterModule::ParameterChanged(int parameter_id)
{
    if(parameter_id == 0)
    {
        cutoff_norm_ = GetParameterAsFloat(0);
        UpdateFilters();
    }
    else if(parameter_id == 1)
    {
        hp_mode_ = GetParameterAsBool(1);
        // No need to reconfig filters, only routing changes.
    }
}

void FilterModule::ProcessMono(float in)
{
    BaseEffectModule::ProcessMono(in);

    if(!m_isEnabled)
    {
        m_audioLeft  = in;
        m_audioRight = in;
        return;
    }

    float out;
    if(hp_mode_)
        out = hp_filter_(in); // high-pass
    else
        out = lp_filter_(in); // low-pass

    // Mono effect: same on both channels
    m_audioLeft  = out;
    m_audioRight = out;
}

void FilterModule::ProcessStereo(float inL, float inR)
{
    // Treat filter as mono: use left input, output mono on both channels
    ProcessMono(inL);
}

float FilterModule::GetBrightnessForLED(int led_id) const
{
    // Example: LED0 on when HP, LED1 on when LP
    if(led_id == 0)
        return hp_mode_ ? 1.0f : 0.0f;
    if(led_id == 1)
        return hp_mode_ ? 0.0f : 1.0f;
    return 0.0f;
}
