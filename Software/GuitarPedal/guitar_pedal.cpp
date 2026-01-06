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

// Hardware & Core Objects
GuitarPedal125B g_hardware;
constexpr size_t kBlockSize = 48;
CpuLoadMeter g_cpuLoadMeter;

// Effect Chain State
struct {
    std::vector<BaseEffectModule*> chain;
    LooperModule* looper = nullptr;
    bool preFXmode = false;
} g_effects;

// Bypass/Mute State Machine
struct {
    bool effectOn = false;
    bool muteOn = false;
    float muteOffTransitionTimeInSeconds = 0.02f;
    int muteOffTransitionTimeInSamples;
    int samplesTilMuteOff;

    bool bypassOn = false;
    float bypassToggleTransitionTimeInSeconds = 0.01f;
    int bypassToggleTransitionTimeInSamples;
    int samplesTilBypassToggle;

    // Quick-switch debounce
    bool ignoreBypassSwitchUntilNextActuation = false;
    bool effectActiveBeforeQuickSwitch = false;
} g_bypass;

// Crossfade State
struct {
    bool isCrossFading = false;
    bool isCrossFadingForward = true;
    float transitionTimeInSeconds = 0.1f;
    int transitionTimeInSamples;
    int samplesTilComplete;
    CrossFade left, right;
} g_crossfade;

// General Timing
struct {
    uint32_t lastTimeStampUS;
    float secondsSinceStartup = 0.0f;
} g_timing;

// Knob Monitoring
struct {
    bool initialized = false;
    float deadZone = 0.05f;
    float changeTolerance = 1.0f / 256.0f;
    float idleTimeInSeconds = 1.0f;
    volatile bool* cacheChanged = nullptr;
    float* cache = nullptr;
    float* timeTilIdle = nullptr;
} g_knobs;

// Switch Monitoring
struct {
    float idleTimeInSeconds = 2.0f;
    bool* enabledCache = nullptr;
    bool* doubleEnabledCache = nullptr;
    float* timeTilIdle = nullptr;
    bool* heldFired = nullptr;
} g_switches;

// Tempo State
struct {
    bool needToChange = false;
    uint32_t bpm = 0;
} g_tempo;

// MIDI State
struct MidiClockState {
    uint32_t tickCount = 0;
    bool running = false;
};

struct {
    bool enabled = true;
    bool through = true;
    int channel = 1;
    MidiClockState clock;
    bool beatLightOn = false;
} g_midi;

// Knob/Switch Routing Types & State
typedef float (*KnobMapFn)(float);

static constexpr KnobMapFn kDefaultMap = [](float x) {
    return fclamp(x, 0.0f, 1.0f);
};

struct KnobRoute {
    BaseEffectModule* effect;
    int paramId;
    KnobMapFn mapper = kDefaultMap;
};

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
    SwitchAction action;
};

struct {
    std::vector<std::vector<KnobRoute>> knobs;
    std::vector<std::vector<SwitchRoute>> switches;
} g_routing;

// Typical Switch case for Message Type.
void HandleMidiMessage(MidiEvent m) {
    if (!g_hardware.SupportsMidi()) {
        return;
    }

    int channel = 0;

    // Make sure the settings midi channel is within the proper range
    // and convert the channel to be zero indexed instead of 1 like the setting.
    if (g_midi.channel >= 1 && g_midi.channel <= 16) {
        channel = g_midi.channel - 1;
    }

    // Pass the midi message through to midi out if so desired (only handles non system event types)
    if (g_midi.through && m.type < SystemCommon) {
        // Re-pack the Midi Message
        uint8_t midiData[3];

        midiData[0] = 0b10000000 | ((uint8_t)m.type << 4) | ((uint8_t)m.channel);
        midiData[1] = m.data[0];
        midiData[2] = m.data[1];

        int bytesToSend = 3;

        if (m.type == ChannelPressure || m.type == ProgramChange) {
            bytesToSend = 2;
        }

        g_hardware.midi.SendMessage(midiData, sizeof(uint8_t) * bytesToSend);
    }

    if (m.type == SystemRealTime) {
        switch (m.srt_type) {
        case TimingClock:
            if (g_midi.clock.running){
                g_midi.clock.tickCount++;

                if(g_midi.clock.tickCount % 24 == 0) {
                    g_effects.looper->SetClockBeat();
                }

                // detect BEAT here, per tick
                if(g_midi.clock.tickCount % 24 < 8){
                    g_midi.beatLightOn = true;
                } else {
                    g_midi.beatLightOn = false;
                }
            }
            break;

        case Start:
            g_midi.clock.tickCount = 0;
            g_midi.clock.running = true;
            break;

        case Continue:
            g_midi.clock.running = true;
            break;

        case Stop:
            g_midi.clock.running = false;
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
    g_cpuLoadMeter.OnBlockStart();

    // Handle MIDI Events
    if (g_hardware.SupportsMidi() && g_midi.enabled) {
        g_hardware.midi.Listen();

        while (g_hardware.midi.HasEvents()) {
            MidiEvent event = g_hardware.midi.PopEvent();
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
    bool oldEffectOn = g_bypass.effectOn;

    // For looper, we force effect ON. Remove this line for bypassable effects.
    g_bypass.effectOn = true;

    // Handle updating the Hardware Bypass & Muting signals
    if (g_hardware.SupportsTrueBypass()) {
        g_hardware.SetAudioBypass(g_bypass.bypassOn);
        g_hardware.SetAudioMute(g_bypass.muteOn);
    } else {
        g_hardware.SetAudioBypass(false);
        g_hardware.SetAudioMute(false);
    }

    // Handle Effect State being Toggled.
    if (g_bypass.effectOn != oldEffectOn) {
        // Setup the crossfade
        g_crossfade.isCrossFading = true;
        g_crossfade.samplesTilComplete = g_crossfade.transitionTimeInSamples;
        g_crossfade.isCrossFadingForward = g_bypass.effectOn;

        // Start the timing sequence for the Hardware Mute and Relay Bypass.
        if (g_hardware.SupportsTrueBypass()) {
            // Immediately Mute the Output using the Hardware Mute.
            g_bypass.muteOn = true;

            // Set the timing for when the bypass relay should trigger and when to unmute.
            g_bypass.samplesTilMuteOff = g_bypass.muteOffTransitionTimeInSamples;
            g_bypass.samplesTilBypassToggle = g_bypass.bypassToggleTransitionTimeInSamples;
        }
    }

    float crossFadeTargetBuffer[2][kBlockSize];   // actual audio data
    float* crossFadeTarget[2] = { crossFadeTargetBuffer[0], crossFadeTargetBuffer[1] }; // pointers

    for (size_t i = 0; i < size; i++) {
        crossFadeTarget[0][i] = in[0][i];
        crossFadeTarget[1][i] = in[1][i];
    }

    if (g_effects.preFXmode) {
        if (!g_effects.chain.empty() && (g_bypass.effectOn || g_crossfade.isCrossFading)) {
            for (auto* fx : g_effects.chain) {
                if (!fx) continue;
                if (!fx->IsEnabled()) continue;
                if (g_hardware.SupportsStereo()) {
                    fx->ProcessStereoBlock(crossFadeTarget, crossFadeTarget, size);
                } else {
                    fx->ProcessMonoBlock(crossFadeTarget, crossFadeTarget, size);
                }
            }
        }
        if (g_hardware.SupportsStereo()) {
            g_effects.looper->ProcessStereoBlock(crossFadeTarget, crossFadeTarget, size);
        } else {
            g_effects.looper->ProcessMonoBlock(crossFadeTarget, crossFadeTarget, size);
        }
    } else {
        if (g_hardware.SupportsStereo()) {
            g_effects.looper->ProcessStereoBlock(crossFadeTarget, crossFadeTarget, size);
        } else {
            g_effects.looper->ProcessMonoBlock(crossFadeTarget, crossFadeTarget, size);
        }
        if (!g_effects.chain.empty() && (g_bypass.effectOn || g_crossfade.isCrossFading)) {
            for (auto* fx : g_effects.chain) {
                if (!fx) continue;
                if (!fx->IsEnabled()) continue;
                if (g_hardware.SupportsStereo()) {
                    fx->ProcessStereoBlock(crossFadeTarget, crossFadeTarget, size);
                } else {
                    fx->ProcessMonoBlock(crossFadeTarget, crossFadeTarget, size);
                }
            }
        }
    }

    for (size_t i = 0; i < size; i++) {
        if (g_crossfade.isCrossFading) {
            float crossFadeFactor = (float)g_crossfade.samplesTilComplete / (float)g_crossfade.transitionTimeInSamples;

            if (g_crossfade.isCrossFadingForward) {
                crossFadeFactor = 1.0f - crossFadeFactor;
            }

            g_crossfade.left.SetPos(crossFadeFactor);
            g_crossfade.right.SetPos(crossFadeFactor);

            g_crossfade.samplesTilComplete -= 1;

            if (g_crossfade.samplesTilComplete < 0) {
                g_crossfade.isCrossFading = false;
            }
        }

        // Handle Timing for the Hardware Mute and Relay Bypass
        if (g_bypass.muteOn) {
            // Decrement the Sample Counts for the timing of the mute and bypass
            g_bypass.samplesTilMuteOff -= 1;
            g_bypass.samplesTilBypassToggle -= 1;

            // If mute time is up, turn it off.
            if (g_bypass.samplesTilMuteOff < 0) {
                g_bypass.muteOn = false;
            }

            // Toggle the bypass when it's time (needs to be timed to happen while things are muted, or you get an audio pop)
            if (g_bypass.samplesTilBypassToggle < 0) {
                g_bypass.bypassOn = !g_bypass.effectOn;
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

        out[0][i] = g_crossfade.left.Process(crossFadeSourceLeft, crossFadeTargetLeft);
        out[1][i] = g_crossfade.right.Process(crossFadeSourceRight, crossFadeTargetRight);  
    }

    // Update state of the LEDs
    if (g_effects.looper->GetNumRecordedLayers() > 0 || g_effects.looper->IsRecording()) {
        led1Brightness = g_effects.looper->GetBrightnessForLED(0);
    } else {
        led1Brightness = g_midi.beatLightOn ? 1.0f : 0.0f;
    }
    led2Brightness = g_effects.looper->GetBrightnessForLED(1);

    // Handle LEDs
    g_hardware.SetLed(0, led1Brightness);
    g_hardware.SetLed(1, led2Brightness);
    g_hardware.UpdateLeds();

    g_cpuLoadMeter.OnBlockEnd();
}

//======================================================================
//                               MAIN SECTION
//======================================================================
int main(void) {
    const bool boost = false; // true enables cpu boost (480Mhz instead of 400Mhz)

    g_hardware.Init(kBlockSize, boost);

    const float sample_rate = g_hardware.AudioSampleRate();

    // Setup CPU logging of the audio callback
    g_cpuLoadMeter.Init(sample_rate, kBlockSize);

    // Set the number of samples to use for the crossfade based on the hardware sample rate
    g_bypass.muteOffTransitionTimeInSamples = g_hardware.GetNumberOfSamplesForTime(g_bypass.muteOffTransitionTimeInSeconds);
    g_bypass.bypassToggleTransitionTimeInSamples = g_hardware.GetNumberOfSamplesForTime(g_bypass.bypassToggleTransitionTimeInSeconds);
    g_crossfade.transitionTimeInSamples = g_hardware.GetNumberOfSamplesForTime(g_crossfade.transitionTimeInSeconds);

    auto* looper        = new LooperModule();
    auto* delay         = new DelayModule();
    auto* distortion    = new DistortionModule();
    auto* pre_eq        = new FilterModule();
    auto* post_eq       = new FilterModule();
    auto* pitch_router  = new EffectRouterModule();
    auto* pitch_shifter = new PitchShifterModule();

    // Fix some effect parameters
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

    g_effects.looper = looper;

    g_effects.chain.push_back(pitch_router);
    g_effects.chain.push_back(delay);
    g_effects.chain.push_back(pre_eq);
    g_effects.chain.push_back(distortion);
    g_effects.chain.push_back(post_eq);
    
    g_effects.looper->Init(sample_rate);

    for (auto* effect : g_effects.chain) {
        effect->Init(sample_rate);
    }

    // Also init the wrapped pitch shifter
    pitch_shifter->Init(sample_rate);
    // Connect router to the inner pitch-shifter
    pitch_router->SetInner(pitch_shifter);

    // Size the routes to the real knob count
    const int knobCount = g_hardware.GetParameterControlCount();
    g_routing.knobs.resize(knobCount);

    // Size the routes to the real switches count
    g_routing.switches.resize(g_hardware.GetSwitchCount());

    // Setup knob routes
    g_routing.knobs[0].push_back({looper, LooperModule::LAYER});

    g_routing.knobs[1].push_back({looper, LooperModule::FADING, [](float x) { return (1.0f - x); }});

    g_routing.knobs[2].push_back({looper, LooperModule::SPEED});
    g_routing.knobs[2].push_back({pitch_shifter, PitchShifterModule::DIRECTION});
    g_routing.knobs[2].push_back({pitch_shifter, PitchShifterModule::SEMITONE, [](float x) { return x >= 0.5f ? 2 * (x - 0.5f) : 2 * (0.5f - x); }});

    g_routing.knobs[3].push_back({looper, LooperModule::SLICE});

    g_routing.knobs[4].push_back({delay, DelayModule::MOD_AMPLITUDE});
    g_routing.knobs[4].push_back({delay, DelayModule::DELAY_MIX, [](float x) { return x == 0.0f ? 0.0f : 1.0f; }});

    g_routing.knobs[5].push_back({distortion, DistortionModule::GAIN});
    

    /*g_routing.knobs[0].push_back({delay, DelayModule::DELAY_MIX});
    g_routing.knobs[1].push_back({delay, DelayModule::DELAY_TIME});
    g_routing.knobs[2].push_back({delay, DelayModule::D_FEEDBACK});
    g_routing.knobs[3].push_back({delay, DelayModule::MOD_AMPLITUDE});
    g_routing.knobs[4].push_back({delay, DelayModule::MOD_FREQ});*/

    /*g_routing.knobs[0].push_back({distortion, DistortionModule::GAIN});
    g_routing.knobs[1].push_back({distortion, DistortionModule::MIX, [](float x) { return powf(x, 0.7f); }});
    g_routing.knobs[2].push_back({distortion, DistortionModule::INTENSITY});
    g_routing.knobs[3].push_back({post_eq, FilterModule::CUTOFF});*/

    /*g_routing.knobs[1].push_back({pitch_shifter, PitchShifterModule::CROSSFADE});
    g_routing.knobs[3].push_back({pitch_shifter, PitchShifterModule::MODE});
    g_routing.knobs[4].push_back({pitch_shifter, PitchShifterModule::SHIFT});
    g_routing.knobs[5].push_back({pitch_shifter, PitchShifterModule::RETURN});*/

    int altSwitchID         = g_hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Alternate);
    int bypassSwitchID      = g_hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Bypass);
    int triswitch_0_left    = 2;
    int triswitch_0_right   = 3;
    int triswitch_1_left    = 4;
    int triswitch_1_right   = 5;
    int triswitch_2_left    = 6;
    int triswitch_2_right   = 7;

    // Alternate footswitch: toggle delay pressed & looper held
    g_routing.switches[altSwitchID].push_back({looper, SwitchAction::AltPressed});
    g_routing.switches[altSwitchID].push_back({looper, SwitchAction::AltHeld1s});

    g_routing.switches[altSwitchID].push_back({delay, SwitchAction::BypassPressed});
    g_routing.switches[altSwitchID].push_back({pre_eq, SwitchAction::BypassPressed});
    g_routing.switches[altSwitchID].push_back({distortion, SwitchAction::BypassPressed});
    g_routing.switches[altSwitchID].push_back({post_eq, SwitchAction::BypassPressed});
    g_routing.switches[altSwitchID].push_back({pitch_shifter, SwitchAction::BypassPressed});

    // Main/bypass footswitch
    g_routing.switches[bypassSwitchID].push_back({looper, SwitchAction::BypassPressed});

    // Triswitch 1 left: ON/OFF for pitch-shifter routing
    g_routing.switches[triswitch_1_left].push_back({looper, SwitchAction::Id2Pressed});
    g_routing.switches[triswitch_1_left].push_back({looper, SwitchAction::Id2Released});
    g_routing.switches[triswitch_1_left].push_back({pitch_router, SwitchAction::AltPressed});
    g_routing.switches[triswitch_1_left].push_back({pitch_router, SwitchAction::AltReleased});

    // Triswitch 2: left = pre-fx, right/mid = post-fx
    g_routing.switches[triswitch_2_left].push_back({nullptr, SwitchAction::PrePostModeSelect});

    // Setup Relay Bypass State
    if (g_hardware.SupportsTrueBypass()) {
        g_bypass.bypassOn = true;
    }

    // Init the Knob Monitoring System
    g_knobs.cacheChanged = new bool[g_hardware.GetParameterControlCount()];
    g_knobs.cache = new float[g_hardware.GetParameterControlCount()];
    g_knobs.timeTilIdle = new float[g_hardware.GetParameterControlCount()];

    // Init the Switch Monitoring System
    g_switches.enabledCache = new bool[g_hardware.GetSwitchCount()];
    g_switches.doubleEnabledCache = new bool[g_hardware.GetSwitchCount()];
    g_switches.timeTilIdle = new float[g_hardware.GetSwitchCount()];
    g_switches.heldFired = new bool[g_hardware.GetSwitchCount()];

    for (int i = 0; i < g_hardware.GetSwitchCount(); i++) {
        g_switches.enabledCache[i] = false;
        g_switches.doubleEnabledCache[i] = false;
        g_switches.timeTilIdle[i] = 0;
        g_switches.heldFired[i] = false;
    }

    // Setup the cross fader
    g_crossfade.left.Init();
    g_crossfade.right.Init();
    g_crossfade.left.SetPos(0.0f);
    g_crossfade.right.SetPos(0.0f);

    // start callback
    g_hardware.StartAdc();
    g_hardware.StartAudio(AudioCallback);

    // Set initial time stamp
    g_timing.lastTimeStampUS = System::GetUs();

    // Setup Debug Logging
    g_hardware.seed.StartLog();

    uint32_t last_print = 0;

    while (1) {
        // Handle Clock Time
        uint32_t currentTimeStampUS = System::GetUs();
        uint32_t elapsedTimeStampUS = currentTimeStampUS - g_timing.lastTimeStampUS;
        g_timing.lastTimeStampUS = currentTimeStampUS;
        float elapsedTimeInSeconds = (elapsedTimeStampUS / 1000000.0f);
        g_timing.secondsSinceStartup = g_timing.secondsSinceStartup + elapsedTimeInSeconds;

        // print every 500 ms
        if(currentTimeStampUS - last_print > 500000)
        {
            last_print = currentTimeStampUS;
            int avg = (int)(g_cpuLoadMeter.GetAvgCpuLoad() * 100.0f + 0.5f);
            int minv = (int)(g_cpuLoadMeter.GetMinCpuLoad() * 100.0f + 0.5f);
            int maxv = (int)(g_cpuLoadMeter.GetMaxCpuLoad() * 100.0f + 0.5f);

            //g_hardware.seed.PrintLine("CPU avg: %d%%  min: %d%%  max: %d%%", avg, minv, maxv);
            g_hardware.seed.PrintLine("tick %d%%  odd: %d%%", g_midi.clock.tickCount, g_midi.beatLightOn);
        }

        g_effects.looper->SetParameterAsBool(LooperModule::MIDI_SYNC, g_midi.clock.running); 

        // Run polling action.
        bool res = false;
        for (auto* effect : g_effects.chain) {
            if (!effect) continue;
            res |= effect->Poll();
        }
        g_effects.looper->Poll();

        // Handle Knob Changes
        if (!g_knobs.initialized && g_timing.secondsSinceStartup > 1.0f) {
            // Let the initial readings of the knob values settle before trying to use them.
            g_knobs.initialized = true;
        }

        // Handle Inputs
        g_hardware.ProcessAnalogControls();
        g_hardware.ProcessDigitalControls();

        // Process the Pots
        float knobValueRaw;

        for (int i = 0; i < g_hardware.GetParameterControlCount(); i++) {
            knobValueRaw = g_hardware.GetParameterControlValue(i);

            // Knobs don't perfectly return values in the 0.0f - 1.0f range
            // so we will add some deadzone to either end of the knob and remap values into
            // a full 0.0f - 1.0f range.
            if (knobValueRaw < g_knobs.deadZone) {
                knobValueRaw = 0.0f;
            } else if (knobValueRaw > (1.0f - g_knobs.deadZone)) {
                knobValueRaw = 1.0f;
            } else {
                knobValueRaw = (knobValueRaw - g_knobs.deadZone) / (1.0f - (2.0f * g_knobs.deadZone));
            }

            if (!g_knobs.initialized) {
                // Initialize the knobs for the first time to whatever the current knob placements are
                g_knobs.cacheChanged[i] = true;
                g_knobs.timeTilIdle[i] = 0;
                g_knobs.cache[i] = knobValueRaw;
            } else {
                // If the knobs are initialized handle monitor them for changes.
                if (g_knobs.timeTilIdle[i] > 0) {
                    g_knobs.timeTilIdle[i] -= elapsedTimeInSeconds;

                    if (g_knobs.timeTilIdle[i] <= 0) {
                        g_knobs.timeTilIdle[i] = 0;
                        g_knobs.cacheChanged[i] = false;
                    }
                }

                bool knobValueChangedToleranceMet = false;

                if (knobValueRaw > (g_knobs.cache[i] + g_knobs.changeTolerance) ||
                    knobValueRaw < (g_knobs.cache[i] - g_knobs.changeTolerance)) {
                    knobValueChangedToleranceMet = true;
                    g_knobs.cacheChanged[i] = true;
                    g_knobs.timeTilIdle[i] = g_knobs.idleTimeInSeconds;
                }

                if (knobValueChangedToleranceMet || g_knobs.cacheChanged[i]) {
                    g_knobs.cache[i] = knobValueRaw;
                }
            }
        }

        // Process the switches
        for (int sw = 0; sw < g_hardware.GetSwitchCount(); ++sw) {
            bool switchPressed  = g_hardware.switches[sw].RisingEdge();
            bool switchReleased = g_hardware.switches[sw].FallingEdge();
            bool switchHeld  = g_hardware.switches[sw].TimeHeldMs() >= 1000.f;

            // Dispatch all routes for this switch //TODO: add safety in case r.effect is nullptr but keep it possible for PostPreFX select.
            for (const auto &r : g_routing.switches[sw]) {
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
                            g_switches.heldFired[sw] = false;
                        }
                        break;

                    case SwitchAction::BypassReleased:
                        if (switchReleased) {
                            r.effect->BypassFootswitchReleased();

                            // Reset held flag so future holds can fire
                            g_switches.heldFired[sw] = false;
                        }
                        break;

                    case SwitchAction::Id2Released:
                        if (switchReleased) {
                            r.effect->FootswitchReleased(2);

                            // Reset held flag so future holds can fire
                            g_switches.heldFired[sw] = false;
                        }
                        break;

                    // HELD (1s) -> fire once when hold threshold reached, guarded by switchesHeldFired
                    case SwitchAction::AltHeld1s:
                        if (switchHeld && !g_switches.heldFired[sw]) {
                            r.effect->AlternateFootswitchHeldFor1Second();
                            g_switches.heldFired[sw] = true; // prevent repeated calls until release
                        }
                        break;

                    case SwitchAction::BypassHeld1s:
                        if (switchHeld && !g_switches.heldFired[sw]) {
                            r.effect->BypassFootswitchHeldFor1Second();
                            g_switches.heldFired[sw] = true; // prevent repeated calls until release
                        }
                        break;

                    case SwitchAction::PrePostModeSelect:
                        if (switchPressed) {
                            g_effects.preFXmode = true;
                        }
                        if (switchReleased) {
                            g_effects.preFXmode = false;
                        }
                        break;
                }
            }

            // Ensure the held-flag is cleared if user releases the button without any 'Released' route mapped
            // (keeps held-guard consistent even if no route calls reset it)
            if (switchReleased) {g_switches.heldFired[sw] = false;}

            if (g_switches.enabledCache[sw] == true) {
                g_switches.timeTilIdle[sw] -= elapsedTimeInSeconds;

                if (g_switches.timeTilIdle[sw] <= 0) {
                    g_switches.enabledCache[sw] = false;

                    if (g_switches.doubleEnabledCache[sw] != true) {
                        // We can safely know this was only a single tap here.
                    }

                    g_switches.doubleEnabledCache[sw] = false;
                }
            }

            if (switchPressed) {
                // Note that switch is pressed and reset the IdleTimer for detecting double presses
                g_switches.enabledCache[sw] = switchPressed;

                if (g_switches.timeTilIdle[sw] > 0) {
                    g_switches.doubleEnabledCache[sw] = true;
                }

                g_switches.timeTilIdle[sw] = g_switches.idleTimeInSeconds;
            }
        }

        if (g_knobs.initialized) {
            // Only iterate the real knobs reported by hardware
            for (int k = 0; k < knobCount; ++k) {
                if (!g_knobs.cacheChanged[k]) continue;

                float v = g_knobs.cache[k]; // normalized 0..1 after deadzone mapping

                // Send to all mapped targets of knob k
                for (const KnobRoute &r : g_routing.knobs[k]) {
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
