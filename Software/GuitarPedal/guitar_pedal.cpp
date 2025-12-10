#include "daisysp.h"
#include "daisy_seed.h"
#include <string.h>
#include "Hardware-Modules/guitar_pedal_125b.h"
#include "Effect-Modules/base_effect_module.h"
#include "Effect-Modules/delay_module.h"
#include "Effect-Modules/looper_module.h"
#include "Effect-Modules/distortion_module.h"
#include "Effect-Modules/filter_module.h"
#include "Effect-Modules/pitch_shifter_module.h"
#include "Effect-Modules/effect_router_module.h"
#include "Util/audio_utilities.h"
#include <vector>

using namespace daisy;
using namespace daisysp;
using namespace bkshepherd;

GuitarPedal125B hardware;
constexpr size_t kBlockSize = 48;

// Effect Related Variables
std::vector<BaseEffectModule*> effectChain;
LooperModule* glooper = nullptr;
bool preFXmode = false;

// Hardware Related Variables
bool effectOn = false;
bool muteOn = false;
float muteOffTransitionTimeInSeconds = 0.02f;
int muteOffTransitionTimeInSamples;
int samplesTilMuteOff;

bool bypassOn = false;
float bypassToggleTransitionTimeInSeconds = 0.01f;
int bypassToggleTransitionTimeInSamples;
int samplesTilBypassToggle;

uint32_t lastTimeStampUS;
float secondsSinceStartup = 0.0f;

// Used to debounce quick switching to/from the tuner
bool ignoreBypassSwitchUntilNextActuation = false;
bool effectActiveBeforeQuickSwitch = false;

// Pot Monitoring Variables
bool knobValuesInitialized = false;
float knobValueDeadZone = 0.05f; // Dead zone on both ends of the raw knob range
float knobValueChangeTolerance = 1.0f / 256.0f;
float knobValueIdleTimeInSeconds = 1.0f;
volatile bool *knobValueCacheChanged = nullptr;
float *knobValueCache = nullptr;
float *knobValueTimeTilIdle = nullptr;

// Switch Monitoring Variables
float switchEnabledIdleTimeInSeconds = 2.0f;
bool *switchEnabledCache = nullptr;
bool *switchDoubleEnabledCache = nullptr;
float *switchEnabledTimeTilIdle = nullptr;
bool *switchesHeldFired = nullptr;

// Tempo
bool needToChangeTempo = false;
uint32_t globalTempoBPM = 0;

// Midi
bool globalMidiEnabled = true;
bool globalMidiThrough = true;
int globalMidiChannel = 1;
struct MidiClockState
{
    uint32_t tickCount = 0;   // how many TimingClock ticks since Start
    bool     running    = false;
};
MidiClockState globalClock;
bool odd_beat = false;

bool isCrossFading = false;
bool isCrossFadingForward = true; // True goes Source->Target, False goes Target->Source
CrossFade crossFaderLeft, crossFaderRight;
float crossFaderTransitionTimeInSeconds = 0.1f;
int crossFaderTransitionTimeInSamples;
int samplesTilCrossFadingComplete;
CpuLoadMeter cpuLoadMeter;

typedef float (*KnobMapFn)(float);

static constexpr KnobMapFn kDefaultMap = [](float x) {
    return fclamp(x, 0.0f, 1.0f);
};

struct KnobRoute {
    BaseEffectModule*   effect;
    int                 paramId;   // index into that effect's parameter array
    KnobMapFn           mapper = kDefaultMap;   // default linear
};

std::vector<std::vector<KnobRoute>> knobRoutes;

enum class SwitchAction {
    AltPressed,
    AltReleased,
    AltHeld1s,

    BypassPressed,
    BypassReleased,
    BypassHeld1s,

    Id2Pressed,
    Id2Released,

    PrePostModeSelect
};

struct SwitchRoute {
    BaseEffectModule* effect;
    SwitchAction      action;
};

std::vector<std::vector<SwitchRoute>> switchRoutes;

// Typical Switch case for Message Type.
void HandleMidiMessage(MidiEvent m) {
    if (!hardware.SupportsMidi()) {
        return;
    }

    int channel = 0;

    // Make sure the settings midi channel is within the proper range
    // and convert the channel to be zero indexed instead of 1 like the setting.
    if (globalMidiChannel >= 1 && globalMidiChannel <= 16) {
        channel = globalMidiChannel - 1;
    }

    // Pass the midi message through to midi out if so desired (only handles non system event types)
    if (globalMidiThrough && m.type < SystemCommon) {
        // Re-pack the Midi Message
        uint8_t midiData[3];

        midiData[0] = 0b10000000 | ((uint8_t)m.type << 4) | ((uint8_t)m.channel);
        midiData[1] = m.data[0];
        midiData[2] = m.data[1];

        int bytesToSend = 3;

        if (m.type == ChannelPressure || m.type == ProgramChange) {
            bytesToSend = 2;
        }

        hardware.midi.SendMessage(midiData, sizeof(uint8_t) * bytesToSend);
    }

    if (m.type == SystemRealTime) {
        switch (m.srt_type) {
        case TimingClock:
            if(globalClock.running)
                globalClock.tickCount++;

                // detect BEAT here, per tick
                if(globalClock.tickCount % 24 == 0)
                {
                    odd_beat = !odd_beat; // toggle every quarter note
                }
            break;

        case Start:
            globalClock.tickCount = 0;
            globalClock.running    = true;
            break;

        case Continue:
            globalClock.running    = true;
            break;

        case Stop:
            globalClock.running    = false;
            break;

        default:
            // ignore others
            break; 
        }
    }

    // Only listen to messages for the devices set channel.
    if (m.channel != channel) {
        return;
    }

    switch (m.type) {
    case NoteOn: {
        /*if (activeEffect != NULL) {
            NoteOnEvent p = m.AsNoteOn();
            activeEffect->OnNoteOn(p.note, p.velocity);
        }*/
        break;
    }
    case NoteOff: {
        /*if (activeEffect != NULL) {
            NoteOnEvent p = m.AsNoteOn();
            activeEffect->OnNoteOff(p.note, p.velocity);
        }*/
        break;
    }
    case ControlChange: {
        /*if (activeEffect != nullptr) {
            ControlChangeEvent p = m.AsControlChange();

            // Notify the activeEffect to handle this midi cc / value
            activeEffect->MidiCCValueNotification(p.control_number, p.value);

            // Notify the UI to update if this CC message was mapped to an EffectParameter
            int effectParamID = activeEffect->GetMappedParameterIDForMidiCC(p.control_number);

            if (effectParamID != -1) {
                guitarPedalUI.UpdateActiveEffectParameterValue(effectParamID, true);
            }
        }*/
        break;
    }
    case ProgramChange: {
        /*ProgramChangeEvent p = m.AsProgramChange();

        if (p.program >= 0 && p.program < availableEffectsCount) {
            SetActiveEffect(p.program);
        }*/
        break;
    }
    default:
        break;
    }
}

//======================================================================
//                            AUDIO CALLBACK
//======================================================================
static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    cpuLoadMeter.OnBlockStart();

    // Handle MIDI Events
    if (hardware.SupportsMidi() && globalMidiEnabled) {
        hardware.midi.Listen();

        while (hardware.midi.HasEvents()) {
            MidiEvent event = hardware.midi.PopEvent();
            HandleMidiMessage(event);
        }
    }

    // Process Audio
    float inputLeft;
    float inputRight;

    // Default LEDs are off
    float led1Brightness = 0.0f;
    float led2Brightness = 0.0f;

    // Store the previous value of the effect bypass so that we can determine if
    // we need to perform a toggle at the end of processing the switches
    bool oldEffectOn = effectOn;

    // For looper, we force effect ON. Remove this line for bypassable effects.
    effectOn = true;

    // Handle updating the Hardware Bypass & Muting signals
    if (hardware.SupportsTrueBypass()) {
        hardware.SetAudioBypass(bypassOn);
        hardware.SetAudioMute(muteOn);
    } else {
        hardware.SetAudioBypass(false);
        hardware.SetAudioMute(false);
    }

    // Handle Effect State being Toggled.
    if (effectOn != oldEffectOn) {
        // Setup the crossfade
        isCrossFading = true;
        samplesTilCrossFadingComplete = crossFaderTransitionTimeInSamples;
        isCrossFadingForward = effectOn;

        // Start the timing sequence for the Hardware Mute and Relay Bypass.
        if (hardware.SupportsTrueBypass()) {
            // Immediately Mute the Output using the Hardware Mute.
            muteOn = true;

            // Set the timing for when the bypass relay should trigger and when to unmute.
            samplesTilMuteOff = muteOffTransitionTimeInSamples;
            samplesTilBypassToggle = bypassToggleTransitionTimeInSamples;
        }
    }

    float crossFadeTargetBuffer[2][kBlockSize];   // actual audio data
    float* crossFadeTarget[2] = { crossFadeTargetBuffer[0], crossFadeTargetBuffer[1] }; // pointers

    for (size_t i = 0; i < size; i++) {
        crossFadeTarget[0][i] = in[0][i];
        crossFadeTarget[1][i] = in[1][i];
    }

    if (preFXmode) {
        if (!effectChain.empty() && (effectOn || isCrossFading)) {
            for (auto* fx : effectChain) {
                if (!fx) continue;
                if (!fx->IsEnabled()) continue;
                if (hardware.SupportsStereo()) {
                    fx->ProcessStereoBlock(crossFadeTarget, crossFadeTarget, size);
                } else {
                    fx->ProcessMonoBlock(crossFadeTarget, crossFadeTarget, size);
                }
            }
        }
        if (hardware.SupportsStereo()) {
            glooper->ProcessStereoBlock(crossFadeTarget, crossFadeTarget, size);
        } else {
            glooper->ProcessMonoBlock(crossFadeTarget, crossFadeTarget, size);
        }
    } else {
        if (hardware.SupportsStereo()) {
            glooper->ProcessStereoBlock(crossFadeTarget, crossFadeTarget, size);
        } else {
            glooper->ProcessMonoBlock(crossFadeTarget, crossFadeTarget, size);
        }
        if (!effectChain.empty() && (effectOn || isCrossFading)) {
            for (auto* fx : effectChain) {
                if (!fx) continue;
                if (!fx->IsEnabled()) continue;
                if (hardware.SupportsStereo()) {
                    fx->ProcessStereoBlock(crossFadeTarget, crossFadeTarget, size);
                } else {
                    fx->ProcessMonoBlock(crossFadeTarget, crossFadeTarget, size);
                }
            }
        }
    }

    for (size_t i = 0; i < size; i++) {
        if (isCrossFading) {
            float crossFadeFactor = (float)samplesTilCrossFadingComplete / (float)crossFaderTransitionTimeInSamples;

            if (isCrossFadingForward) {
                crossFadeFactor = 1.0f - crossFadeFactor;
            }

            crossFaderLeft.SetPos(crossFadeFactor);
            crossFaderRight.SetPos(crossFadeFactor);

            samplesTilCrossFadingComplete -= 1;

            if (samplesTilCrossFadingComplete < 0) {
                isCrossFading = false;
            }
        }

        // Handle Timing for the Hardware Mute and Relay Bypass
        if (muteOn) {
            // Decrement the Sample Counts for the timing of the mute and bypass
            samplesTilMuteOff -= 1;
            samplesTilBypassToggle -= 1;

            // If mute time is up, turn it off.
            if (samplesTilMuteOff < 0) {
                muteOn = false;
            }

            // Toggle the bypass when it's time (needs to be timed to happen while things are muted, or you get an audio pop)
            if (samplesTilBypassToggle < 0) {
                bypassOn = !effectOn;
            }
        }

        // Handle Mono vs Stereo
        inputLeft = in[0][i];
        inputRight = in[1][i];

        float crossFadeSourceLeft  = inputLeft;
        float crossFadeSourceRight = inputRight;
        float crossFadeTargetLeft  = inputLeft;
        float crossFadeTargetRight = inputRight;

        crossFadeTargetLeft  = crossFadeTarget[0][i];
        crossFadeTargetRight = crossFadeTarget[1][i];

        out[0][i] = crossFaderLeft.Process(crossFadeSourceLeft, crossFadeTargetLeft);
        out[1][i] = crossFaderRight.Process(crossFadeSourceRight, crossFadeTargetRight);  
    }

    // Update state of the LEDs
    /*led1Brightness = glooper->GetBrightnessForLED(0);
    led2Brightness = glooper->GetBrightnessForLED(1); */

    led1Brightness = odd_beat ? 1.0f : 0.0f;
    led2Brightness = odd_beat ? 0.0f : 1.0f;

    // Handle LEDs
    hardware.SetLed(0, led1Brightness);
    hardware.SetLed(1, led2Brightness);
    hardware.UpdateLeds();

    cpuLoadMeter.OnBlockEnd();
}

//======================================================================
//                               MAIN SECTION
//======================================================================
int main(void) {
    const bool boost = false; // true enables cpu boost (480Mhz instead of 400Mhz)

    hardware.Init(kBlockSize, boost);

    const float sample_rate = hardware.AudioSampleRate();

    // Setup CPU logging of the audio callback
    cpuLoadMeter.Init(sample_rate, kBlockSize);

    // Set the number of samples to use for the crossfade based on the hardware sample rate
    muteOffTransitionTimeInSamples = hardware.GetNumberOfSamplesForTime(muteOffTransitionTimeInSeconds);
    bypassToggleTransitionTimeInSamples = hardware.GetNumberOfSamplesForTime(bypassToggleTransitionTimeInSeconds);
    crossFaderTransitionTimeInSamples = hardware.GetNumberOfSamplesForTime(crossFaderTransitionTimeInSeconds);

    auto* looper        = new LooperModule();
    auto* delay         = new DelayModule();
    auto* distortion    = new DistortionModule();
    auto* pre_eq        = new FilterModule();
    auto* post_eq       = new FilterModule();
    auto* pitch_router  = new EffectRouterModule();
    auto* pitch_shifter = new PitchShifterModule();

    // Fix some effect parameters
    // delay->SetParameterAsMagnitude(DelayModule::DELAY_MIX, 1.0f);
    delay->SetParameterAsMagnitude(DelayModule::DELAY_LPF, 1.0f);
    delay->SetParameterAsMagnitude(DelayModule::DELAY_TIME, 0.0f);
    delay->SetParameterAsMagnitude(DelayModule::D_FEEDBACK, 0.0f);
    delay->SetParameterAsMagnitude(DelayModule::DELAY_MIX, 1.0f);
    delay->SetParameterAsBinnedValue(DelayModule::MOD_PARAM, 2);
    delay->SetParameterAsBinnedValue(DelayModule::MOD_WAVE, 6);
    delay->SetParameterAsBinnedValue(DelayModule::MOD_FREQ, 0.65f);

    distortion->SetParameterAsMagnitude(DistortionModule::LEVEL, 1.0f);
    distortion->SetParameterAsMagnitude(DistortionModule::TONE, 0.50f);
    distortion->SetParameterAsBool(DistortionModule::OVERSAMP, 0);
    distortion->SetParameterAsBinnedValue(DistortionModule::DIST_TYPE, 5);

    pre_eq->SetParameterAsBool(FilterModule::HP_MODE, true);
    pre_eq->SetParameterAsFloat(FilterModule::CUTOFF, 0.0f);
    post_eq->SetParameterAsBool(FilterModule::HP_MODE, false);
    post_eq->SetParameterAsFloat(FilterModule::CUTOFF, 0.96f);

    pitch_shifter->SetParameterAsBinnedValue(PitchShifterModule::MODE, 1); //Latching
    pitch_shifter->SetParameterAsFloat(PitchShifterModule::CROSSFADE, 1.0f);

    looper->SetEnabled(true);
    delay->SetEnabled(false);
    pre_eq->SetEnabled(false);
    distortion->SetEnabled(false);
    post_eq->SetEnabled(false);
    pitch_shifter->SetEnabled(false);
    pitch_router->SetEnabled(true);   // router must always run

    glooper = looper;

    effectChain.push_back(delay);
    effectChain.push_back(pre_eq);
    effectChain.push_back(distortion);
    effectChain.push_back(post_eq);
    //effectChain.push_back(pitch_router);
    
    glooper->Init(sample_rate);

    for (auto* effect : effectChain) {
        effect->Init(sample_rate);
    }

    // Also init the wrapped pitch shifter
    pitch_shifter->Init(sample_rate);
    // Connect router to the inner pitch-shifter
    pitch_router->SetInner(pitch_shifter);

    // Size the routes to the real knob count
    const int knobCount = hardware.GetParameterControlCount();
    knobRoutes.resize(knobCount);

    // Size the routes to the real switches count
    switchRoutes.resize(hardware.GetSwitchCount());

    // Setup knob routes
    /*knobRoutes[0].push_back({looper, LooperModule::LAYER});

    knobRoutes[1].push_back({looper, LooperModule::FADING, 1.0f, 0.0f});

    knobRoutes[2].push_back({looper, LooperModule::SPEED});
    knobRoutes[2].push_back({pitch_shifter, PitchShifterModule::DIRECTION});
    knobRoutes[2].push_back({pitch_shifter, PitchShifterModule::SEMITONE, 1.0f, -1.0f});
    knobRoutes[2].push_back({pitch_shifter, PitchShifterModule::SEMITONE, -1.0f, 1.0f});

    knobRoutes[3].push_back({looper, LooperModule::SLICE});*/

    knobRoutes[4].push_back({delay, DelayModule::MOD_AMPLITUDE});
    knobRoutes[4].push_back({delay, DelayModule::DELAY_MIX, [](float x) { return x == 0.0f ? 0.0f : 1.0f; }});

    /*knobRoutes[5].push_back({distortion, DistortionModule::GAIN, 0.0f, 0.8f});
    knobRoutes[5].push_back({distortion, DistortionModule::INTENSITY, 0.0f, 0.8f});
    knobRoutes[5].push_back({distortion, DistortionModule::MIX, 0.0f, 0.8f});
    knobRoutes[5].push_back({distortion, DistortionModule::LEVEL, 1.0f, 0.15f});
    knobRoutes[5].push_back({distortion, DistortionModule::TONE, 0.5f, 0.4f});*/

    /*knobRoutes[0].push_back({delay, DelayModule::DELAY_MIX});
    knobRoutes[1].push_back({delay, DelayModule::DELAY_TIME});
    knobRoutes[2].push_back({delay, DelayModule::D_FEEDBACK});
    knobRoutes[3].push_back({delay, DelayModule::MOD_AMPLITUDE});
    knobRoutes[4].push_back({delay, DelayModule::MOD_FREQ});*/

    knobRoutes[0].push_back({distortion, DistortionModule::GAIN});
    knobRoutes[1].push_back({distortion, DistortionModule::MIX, [](float x) { return powf(x, 0.7f); }});
    knobRoutes[2].push_back({distortion, DistortionModule::INTENSITY});
    knobRoutes[3].push_back({post_eq, FilterModule::CUTOFF});

    /*knobRoutes[1].push_back({pitch_shifter, PitchShifterModule::CROSSFADE});
    knobRoutes[3].push_back({pitch_shifter, PitchShifterModule::MODE});
    knobRoutes[4].push_back({pitch_shifter, PitchShifterModule::SHIFT});
    knobRoutes[5].push_back({pitch_shifter, PitchShifterModule::RETURN});*/

    // 1: layers, 2: fading, 3: stability/bitcrusher, 4: slice/stretch, 5: speed/pitch, 6: distortion
    // A: single/all/direct B:fixed/flex C:

    int altSwitchID         = hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Alternate);
    int bypassSwitchID      = hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Bypass);
    int triswitch_0_left    = 2;
    int triswitch_0_right   = 3;
    int triswitch_1_left    = 4;
    int triswitch_1_right   = 5;
    int triswitch_2_left    = 6;
    int triswitch_2_right   = 7;

    // Alternate footswitch: toggle delay pressed & looper held
    switchRoutes[altSwitchID].push_back({looper, SwitchAction::AltPressed});
    switchRoutes[altSwitchID].push_back({looper, SwitchAction::AltHeld1s});

    switchRoutes[altSwitchID].push_back({delay, SwitchAction::BypassPressed});
    switchRoutes[altSwitchID].push_back({pre_eq, SwitchAction::BypassPressed});
    switchRoutes[altSwitchID].push_back({distortion, SwitchAction::BypassPressed});
    switchRoutes[altSwitchID].push_back({post_eq, SwitchAction::BypassPressed});
    switchRoutes[altSwitchID].push_back({pitch_shifter, SwitchAction::BypassPressed});

    // Main/bypass footswitch
    switchRoutes[bypassSwitchID].push_back({looper, SwitchAction::BypassPressed});

    // Triswitch 1 left: ON/OFF for pitch-shifter routing
    switchRoutes[triswitch_1_left].push_back({looper, SwitchAction::Id2Pressed});
    switchRoutes[triswitch_1_left].push_back({looper, SwitchAction::Id2Released});
    switchRoutes[triswitch_1_left].push_back({pitch_router, SwitchAction::AltPressed});
    switchRoutes[triswitch_1_left].push_back({pitch_router, SwitchAction::AltReleased});

    // Triswitch 2: left = pre-fx, right/mid = post-fx
    switchRoutes[triswitch_2_left].push_back({nullptr, SwitchAction::PrePostModeSelect});

    // Setup Relay Bypass State
    if (hardware.SupportsTrueBypass()) {
        bypassOn = true;
    }

    // Init the Knob Monitoring System
    knobValueCacheChanged = new bool[hardware.GetParameterControlCount()];
    knobValueCache = new float[hardware.GetParameterControlCount()];
    knobValueTimeTilIdle = new float[hardware.GetParameterControlCount()];

    // Init the Switch Monitoring System
    switchEnabledCache = new bool[hardware.GetSwitchCount()];
    switchDoubleEnabledCache = new bool[hardware.GetSwitchCount()];
    switchEnabledTimeTilIdle = new float[hardware.GetSwitchCount()];
    switchesHeldFired = new bool[hardware.GetSwitchCount()];

    for (int i = 0; i < hardware.GetSwitchCount(); i++) {
        switchEnabledCache[i] = false;
        switchDoubleEnabledCache[i] = false;
        switchEnabledTimeTilIdle[i] = 0;
        switchesHeldFired[i] = false;
    }

    // Setup the cross fader
    crossFaderLeft.Init();
    crossFaderRight.Init();
    crossFaderLeft.SetPos(0.0f);
    crossFaderRight.SetPos(0.0f);

    // start callback
    hardware.StartAdc();
    hardware.StartAudio(AudioCallback);

    // Set initial time stamp
    lastTimeStampUS = System::GetUs();

    // Setup Debug Logging
    hardware.seed.StartLog();

    uint32_t last_print = 0;

    while (1) {
        // Handle Clock Time
        uint32_t currentTimeStampUS = System::GetUs();
        uint32_t elapsedTimeStampUS = currentTimeStampUS - lastTimeStampUS;
        lastTimeStampUS = currentTimeStampUS;
        float elapsedTimeInSeconds = (elapsedTimeStampUS / 1000000.0f);
        secondsSinceStartup = secondsSinceStartup + elapsedTimeInSeconds;

        // print every 500 ms
        if(currentTimeStampUS - last_print > 500000)
        {
            last_print = currentTimeStampUS;
            int avg = (int)(cpuLoadMeter.GetAvgCpuLoad() * 100.0f + 0.5f);
            int minv = (int)(cpuLoadMeter.GetMinCpuLoad() * 100.0f + 0.5f);
            int maxv = (int)(cpuLoadMeter.GetMaxCpuLoad() * 100.0f + 0.5f);

            //hardware.seed.PrintLine("CPU avg: %d%%  min: %d%%  max: %d%%", avg, minv, maxv);
            hardware.seed.PrintLine("tick %d%%  odd: %d%%", globalClock.tickCount, odd_beat);

        }

        // Run polling action.
        bool res = false;
        for (auto* effect : effectChain) {
            if (!effect) continue;
            res |= effect->Poll();
        }
        glooper->Poll();

        // Handle Knob Changes
        if (!knobValuesInitialized && secondsSinceStartup > 1.0f) {
            // Let the initial readings of the knob values settle before trying to use them.
            knobValuesInitialized = true;
        }

        // Handle Inputs
        hardware.ProcessAnalogControls();
        hardware.ProcessDigitalControls();

        // Process the Pots
        float knobValueRaw;

        for (int i = 0; i < hardware.GetParameterControlCount(); i++) {
            knobValueRaw = hardware.GetParameterControlValue(i);

            // Knobs don't perfectly return values in the 0.0f - 1.0f range
            // so we will add some deadzone to either end of the knob and remap values into
            // a full 0.0f - 1.0f range.
            if (knobValueRaw < knobValueDeadZone) {
                knobValueRaw = 0.0f;
            } else if (knobValueRaw > (1.0f - knobValueDeadZone)) {
                knobValueRaw = 1.0f;
            } else {
                knobValueRaw = (knobValueRaw - knobValueDeadZone) / (1.0f - (2.0f * knobValueDeadZone));
            }

            if (!knobValuesInitialized) {
                // Initialize the knobs for the first time to whatever the current knob placements are
                knobValueCacheChanged[i] = true;
                knobValueTimeTilIdle[i] = 0;
                knobValueCache[i] = knobValueRaw;
            } else {
                // If the knobs are initialized handle monitor them for changes.
                if (knobValueTimeTilIdle[i] > 0) {
                    knobValueTimeTilIdle[i] -= elapsedTimeInSeconds;

                    if (knobValueTimeTilIdle[i] <= 0) {
                        knobValueTimeTilIdle[i] = 0;
                        knobValueCacheChanged[i] = false;
                    }
                }

                bool knobValueChangedToleranceMet = false;

                if (knobValueRaw > (knobValueCache[i] + knobValueChangeTolerance) ||
                    knobValueRaw < (knobValueCache[i] - knobValueChangeTolerance)) {
                    knobValueChangedToleranceMet = true;
                    knobValueCacheChanged[i] = true;
                    knobValueTimeTilIdle[i] = knobValueIdleTimeInSeconds;
                }

                if (knobValueChangedToleranceMet || knobValueCacheChanged[i]) {
                    knobValueCache[i] = knobValueRaw;
                }
            }
        }

        // Process the switches
        for (int sw = 0; sw < hardware.GetSwitchCount(); ++sw) {
            bool switchPressed  = hardware.switches[sw].RisingEdge();
            bool switchReleased = hardware.switches[sw].FallingEdge();
            bool switchHeld  = hardware.switches[sw].TimeHeldMs() >= 1000.f;

            // Dispatch all routes for this switch //TODO: add safety in case r.effect is nullptr but keep it possible for PostPreFX select.
            for (const auto &r : switchRoutes[sw]) {
                switch (r.action) {
                    // SHORT PRESS -> fire on RisingEdge
                    case SwitchAction::AltPressed:
                        if (switchPressed) {
                            r.effect->AlternateFootswitchPressed();
                        }
                        break;

                    case SwitchAction::BypassPressed:
                        if (switchPressed) {
                            r.effect->BypassFootswitchPressed();
                        }
                        break;

                    case SwitchAction::Id2Pressed:
                        if (switchPressed) {
                            r.effect->FootswitchPressed(2);
                        }
                        break;

                    // RELEASE -> fire on FallingEdge (also clear held-guard)
                    case SwitchAction::AltReleased:
                        if (switchReleased) {
                            r.effect->AlternateFootswitchReleased();

                            // Reset held flag so future holds can fire
                            switchesHeldFired[sw] = false;
                        }
                        break;

                    case SwitchAction::BypassReleased:
                        if (switchReleased) {
                            r.effect->BypassFootswitchReleased();

                            // Reset held flag so future holds can fire
                            switchesHeldFired[sw] = false;
                        }
                        break;

                    case SwitchAction::Id2Released:
                        if (switchReleased) {
                            r.effect->FootswitchReleased(2);

                            // Reset held flag so future holds can fire
                            switchesHeldFired[sw] = false;
                        }
                        break;

                    // HELD (1s) -> fire once when hold threshold reached, guarded by switchesHeldFired
                    case SwitchAction::AltHeld1s:
                        if (switchHeld && !switchesHeldFired[sw]) {
                            r.effect->AlternateFootswitchHeldFor1Second();
                            switchesHeldFired[sw] = true; // prevent repeated calls until release
                        }
                        break;

                    case SwitchAction::BypassHeld1s:
                        if (switchHeld && !switchesHeldFired[sw]) {
                            r.effect->BypassFootswitchHeldFor1Second();
                            switchesHeldFired[sw] = true; // prevent repeated calls until release
                        }
                        break;

                    case SwitchAction::PrePostModeSelect:
                        if (switchPressed) {
                            preFXmode = true;
                        }
                        if (switchReleased) {
                            preFXmode = false;
                        }
                        break;
                }
            }

            // Ensure the held-flag is cleared if user releases the button without any 'Released' route mapped
            // (keeps held-guard consistent even if no route calls reset it)
            if (switchReleased) {switchesHeldFired[sw] = false;}

            if (switchEnabledCache[sw] == true) {
                switchEnabledTimeTilIdle[sw] -= elapsedTimeInSeconds;

                if (switchEnabledTimeTilIdle[sw] <= 0) {
                    switchEnabledCache[sw] = false;

                    if (switchDoubleEnabledCache[sw] != true) {
                        // We can safely know this was only a single tap here.
                    }

                    switchDoubleEnabledCache[sw] = false;
                }
            }

            if (switchPressed) {
                // Note that switch is pressed and reset the IdleTimer for detecting double presses
                switchEnabledCache[sw] = switchPressed;

                if (switchEnabledTimeTilIdle[sw] > 0) {
                    switchDoubleEnabledCache[sw] = true;
                }

                switchEnabledTimeTilIdle[sw] = switchEnabledIdleTimeInSeconds;
            }
        }

        if (knobValuesInitialized) {
            // Only iterate the real knobs reported by hardware
            for (int k = 0; k < knobCount; ++k) {
                if (!knobValueCacheChanged[k]) continue;

                float v = knobValueCache[k]; // normalized 0..1 after deadzone mapping

                // Send to all mapped targets of knob k
                for (const KnobRoute &r : knobRoutes[k]) {
                    if (!r.effect) continue;               // safety: null-check
                    if (r.paramId < 0) continue;          // safety: invalid param id

                    float val = r.mapper(v);
                    val = fclamp(val, 0.0f, 1.0f);
                    r.effect->SetParameterAsMagnitude(r.paramId, val);
                }
            }
        }
    }
}
