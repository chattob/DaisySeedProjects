#include "guitar_pedal_125b.h"

using namespace bkshepherd;

static const int s_switchParamCount = 8;
static const PreferredSwitchMetaData s_switchMetaData[s_switchParamCount] = {
    {sfType : SpecialFunctionType::Bypass, switchMapping : 0}, {sfType : SpecialFunctionType::Alternate, switchMapping : 1}};

GuitarPedal125B::GuitarPedal125B() : BaseHardwareModule() {
    // Setup the Switch Meta Data for this hardware
    m_switchMetaDataParamCount = s_switchParamCount;
    m_switchMetaData = s_switchMetaData;
}

GuitarPedal125B::~GuitarPedal125B() {}

void GuitarPedal125B::Init(size_t blockSize, bool boost) {
    BaseHardwareModule::Init(blockSize, boost);

    m_supportsStereo = true;

    Pin knobPins[] = {seed::D15, seed::D16, seed::D17, seed::D18, seed::D19, seed::D20};
    InitKnobs(6, knobPins);

    TriPin tri_pins[] = {
        {seed::D11, seed::D12},
        {seed::D9, seed::D10},
    };
    InitTriSwitches(2, tri_pins);

    Pin switchPins[] = {seed::D7, seed::D8, seed::D13, seed::D14};
    InitSwitches(4, switchPins);

    Pin ledPins[] = {seed::D22, seed::D23};
    InitLeds(2, ledPins);

    InitMidi(seed::D30, seed::D29);
    InitTrueBypass(seed::D0, seed::D25);
}