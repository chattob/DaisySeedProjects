#include "daisysp.h"
#include "daisy_seed.h"
#include <string.h>
#include "Hardware-Modules/guitar_pedal_125b.h"
#include "Effect-Modules/base_effect_module.h"
#include "Effect-Modules/micro_looper_module.h"
#include "Effect-Modules/polyoctave_module.h"
#include "Effect-Modules/delay_module.h"
#include "Effect-Modules/distortion_module.h"
#include "Effect-Modules/crusher_module.h"
#include "Effect-Modules/mixer_module.h"
#include "Effect-Modules/reverb_module.h"
#include "Effect-Modules/pitch_shifter_module.h"
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
    MicroLooperModule* micro_looper = nullptr;
    ReverbModule* reverb = nullptr;
    PitchShifterModule* pitchshifter = nullptr;
    MixerModule* mixer = nullptr;
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
    float changeTolerance = 1.0f / 1024.0f;
    float idleTimeInSeconds = 1.0f;
    volatile bool* cacheChanged = nullptr;
    float* cache = nullptr;
    float* timeTilIdle = nullptr;
} g_knobs;

// Switch Monitoring
struct {
    float idleTimeInSeconds = 0.5f;
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
static constexpr int kMidiCCCount = 128;
static constexpr int kMidiKnobMirrorBaseCC = 14;

static constexpr KnobMapFn kDefaultMap = [](float x) {
    return fclamp(x, 0.0f, 1.0f);
};

struct KnobRoute {
    BaseEffectModule* effect;
    int paramId;
    KnobMapFn mapper = kDefaultMap;
};

enum class SwitchAction {
    Pressed,
    DoubleTapped,
    Released,
    Held1s
};

struct SwitchRoute {
    BaseEffectModule* effect;
    int switchId;
    SwitchAction action;
};

struct {
    std::vector<std::vector<KnobRoute>> knobs;
    std::vector<std::vector<KnobRoute>> midiCC;
    std::vector<std::vector<SwitchRoute>> switches;
} g_routing;

static bool DispatchParamRoutes(const std::vector<KnobRoute>& routes, float normalizedValue) {
    bool handled = false;

    for (const KnobRoute& r : routes) {
        if (!r.effect) continue;
        if (r.paramId < 0) continue;

        float mapped = r.mapper(normalizedValue);
        mapped = fclamp(mapped, 0.0f, 1.0f);
        r.effect->SetParameterAsMagnitude(r.paramId, mapped);
        handled = true;
    }

    return handled;
}

// Typical Switch case for Message Type.
void HandleMidiMessage(MidiEvent m) {
    if (m.type == SystemRealTime) {
        switch (m.srt_type) {
            case TimingClock:
                if (g_midi.clock.running) {
                    g_midi.clock.tickCount++;

                    if (g_effects.micro_looper) {
                        g_effects.micro_looper->OnMidiClockPulse();
                        if ((g_midi.clock.tickCount % 24) == 0) {
                            g_effects.micro_looper->SetClockBeat();
                        }
                    }

                    g_midi.beatLightOn = (g_midi.clock.tickCount % 24) < 8;
                }
                break;

            case Start:
                g_midi.clock.tickCount = 0;
                g_midi.clock.running = true;
                g_midi.beatLightOn = false;
                break;

            case Continue:
                g_midi.clock.running = true;
                break;

            case Stop:
                g_midi.clock.running = false;
                g_midi.beatLightOn = false;
                break;

            default:
                break;
        }
        return;
    }

    // Use channel 1..16 in configuration. Set <=0 for omni mode.
    if (g_midi.channel > 0 && (m.channel + 1) != g_midi.channel) {
        return;
    }

    switch (m.type) {
        case ControlChange: {
            ControlChangeEvent cc = m.AsControlChange();
            if (cc.control_number >= g_routing.midiCC.size()) break;
            float normalizedValue = static_cast<float>(cc.value) / 127.0f;
            DispatchParamRoutes(g_routing.midiCC[cc.control_number], normalizedValue);
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

    led1Brightness = g_effects.micro_looper->GetBrightnessForLED(0);
    led2Brightness = g_effects.micro_looper->GetBrightnessForLED(1);

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

    static float crossFadeTargetBuffer[2][kBlockSize];   // actual audio data
    static float* crossFadeTarget[2] = { crossFadeTargetBuffer[0], crossFadeTargetBuffer[1] }; // pointers

    for (size_t i = 0; i < size; i++) {
        crossFadeTarget[0][i] = in[0][i];
        crossFadeTarget[1][i] = in[1][i];
    }

    // Capture dry signal for mixer channel 1 (channel 0 auto-captures wet)
    g_effects.mixer->ResetCaptures();
    g_effects.mixer->CaptureChannel(1, in, size);

    const bool isStereo = g_hardware.SupportsStereo();

    if (!g_effects.chain.empty() && (g_bypass.effectOn || g_crossfade.isCrossFading)) {
        for (auto* fx : g_effects.chain) {
            if (!fx) continue;
            if (!fx->IsEnabled()) continue;
            fx->BlockPreProcessing(size);
        }

        for (size_t i = 0; i < size; i++) {
            float sampleL = crossFadeTarget[0][i];
            float sampleR = isStereo ? crossFadeTarget[1][i] : sampleL;

            for (auto* fx : g_effects.chain) {
                if (!fx) continue;
                if (!fx->IsEnabled()) continue;

                fx->ProcessStereo(sampleL, sampleR);
                sampleL = fx->GetAudioLeft();
                sampleR = isStereo ? fx->GetAudioRight() : fx->GetAudioLeft();
            }

            crossFadeTarget[0][i] = sampleL;
            crossFadeTarget[1][i] = sampleR;
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
    const bool boost = true; // true enables cpu boost (480Mhz instead of 400Mhz)

    g_hardware.Init(kBlockSize, boost);

    const float sample_rate = g_hardware.AudioSampleRate();

    // Setup CPU logging of the audio callback
    g_cpuLoadMeter.Init(sample_rate, kBlockSize);

    // Set the number of samples to use for the crossfade based on the hardware sample rate
    g_bypass.muteOffTransitionTimeInSamples = g_hardware.GetNumberOfSamplesForTime(g_bypass.muteOffTransitionTimeInSeconds);
    g_bypass.bypassToggleTransitionTimeInSamples = g_hardware.GetNumberOfSamplesForTime(g_bypass.bypassToggleTransitionTimeInSeconds);
    g_crossfade.transitionTimeInSamples = g_hardware.GetNumberOfSamplesForTime(g_crossfade.transitionTimeInSeconds);

    g_effects.micro_looper  = new MicroLooperModule();
    auto polyoctave         = new PolyOctaveModule();
    auto tape               = new DelayModule();
    auto distortion         = new DistortionModule();
    auto crusher            = new CrusherModule();
    g_effects.reverb        = new ReverbModule();
    g_effects.pitchshifter  = new PitchShifterModule();

    g_effects.micro_looper->SetParameterAsBinnedValue(MicroLooperModule::LOOP_MODE, MicroLooperModule::OVERDUB);
    g_effects.micro_looper->SetParameterAsFloat(MicroLooperModule::IN_MIX, 1.0f);

    tape->SetParameterAsMagnitude(DelayModule::DELAY_LPF, 1.0f);
    tape->SetParameterAsMagnitude(DelayModule::DELAY_TIME, 0.0f);
    tape->SetParameterAsMagnitude(DelayModule::D_FEEDBACK, 0.0f);
    tape->SetParameterAsMagnitude(DelayModule::DELAY_MIX, 1.0f);
    tape->SetParameterAsBinnedValue(DelayModule::MOD_PARAM, DelayModule::MOD_DELAY_TIME);
    tape->SetParameterAsBinnedValue(DelayModule::MOD_WAVE, DelayModule::WAVE_PERLIN);
    tape->SetParameterAsMagnitude(DelayModule::MOD_FREQ, 0.65f);

    distortion->SetParameterAsMagnitude(DistortionModule::LEVEL, 1.0f);
    distortion->SetParameterAsMagnitude(DistortionModule::TONE, 0.50f);
    distortion->SetParameterAsBool(DistortionModule::OVERSAMP, 0);
    distortion->SetParameterAsBinnedValue(DistortionModule::DIST_TYPE, 5);

    crusher->SetParameterAsBinnedValue(CrusherModule::BITS, 32);
    crusher->SetParameterAsMagnitude(CrusherModule::MIX, 0.8f);
    crusher->SetParameterAsMagnitude(CrusherModule::CUTOFF, 0.25f);
    crusher->SetParameterAsMagnitude(CrusherModule::FILTER_Q, 0.2f);
    crusher->SetParameterAsMagnitude(CrusherModule::LEVEL, 1.0f);
    crusher->SetParameterAsMagnitude(CrusherModule::JITTER, 0.4f);

    g_effects.pitchshifter->SetParameterAsBool(PitchShifterModule::SMOOTH, true);

    /*reverb->SetParameterAsBool(CloudSeedModule::STEREO_IN, false);
    reverb->SetParameterAsBool(CloudSeedModule::SUM_TO_MONO, false);
    reverb->SetParameterAsFloat(CloudSeedModule::MOD_AMOUNT, 0.0f);
    reverb->SetParameterAsFloat(CloudSeedModule::MOD_RATE, 0.0f);*/
    g_effects.reverb->SetParameterAsFloat(ReverbModule::MIX, 1.0f);
    g_effects.reverb->SetParameterAsFloat(ReverbModule::DAMP, 0.0f);
    //reverb->SetParameterAsFloat(CloudSeedModule::MOD_AMOUNT, 0.0f);

    g_effects.mixer = new MixerModule();

    g_effects.chain.push_back(g_effects.micro_looper);
    g_effects.chain.push_back(g_effects.reverb);
    g_effects.chain.push_back(g_effects.pitchshifter);
    g_effects.chain.push_back(tape);
    g_effects.chain.push_back(distortion);
    g_effects.chain.push_back(crusher);
    g_effects.chain.push_back(g_effects.mixer);  // Mixer last in chain

    for (auto* effect : g_effects.chain) {
        effect->Init(sample_rate);
        effect->SetEnabled(true);
    }
    g_effects.reverb->SetEnabled(false); // Reverb is only enabled in "Reverb" mode.
    g_effects.pitchshifter->SetEnabled(false); // Pitchshifter is only enabled in "Reverb" mode.

    // Size the routes to the real knob count
    const int knobCount = g_hardware.GetParameterControlCount();
    g_routing.knobs.resize(knobCount);
    g_routing.midiCC.resize(kMidiCCCount);

    // Size the routes to the real switches count
    g_routing.switches.resize(g_hardware.GetSwitchCount());

    // Setup knob routes
    g_routing.knobs[0].push_back({g_effects.micro_looper, MicroLooperModule::SENSITIVITY});
    g_routing.knobs[0].push_back({g_effects.micro_looper, MicroLooperModule::FADING});
    g_routing.knobs[0].push_back({g_effects.reverb, ReverbModule::TIME});

    g_routing.knobs[1].push_back({g_effects.micro_looper, MicroLooperModule::ATTACK});
    g_routing.knobs[1].push_back({g_effects.micro_looper, MicroLooperModule::SLICE, [](float x) { return 1.0f/kMicroLoopSliceDiv + x * (kMicroLoopSliceDiv - 1.0f)/kMicroLoopSliceDiv; }});

    g_routing.knobs[2].push_back({g_effects.micro_looper, MicroLooperModule::PITCH_VOICE});
    g_routing.knobs[2].push_back({g_effects.pitchshifter, PitchShifterModule::DIRECTION, [](float x) { return x >= 0.5f ? 1.0f : 0.0f; }});
    g_routing.knobs[2].push_back({g_effects.pitchshifter, PitchShifterModule::PITCH_SHIFT, [](float x) { return x >= 0.5f ? 2.0f * (x - 0.5f) : 2.0f * (0.5f - x); }});

    g_routing.knobs[3].push_back({g_effects.micro_looper, MicroLooperModule::PITCH_MIX});
    g_routing.knobs[3].push_back({g_effects.pitchshifter, PitchShifterModule::MIX});

    g_routing.knobs[4].push_back({tape, DelayModule::MOD_AMPLITUDE});
    g_routing.knobs[4].push_back({tape, DelayModule::DELAY_MIX, [](float x) { return x == 0.0f ? 0.0f : 1.0f; }});

    g_routing.knobs[5].push_back({distortion, DistortionModule::GAIN, [](float x) { return x < 0.5f ? 0.0f : 2.0f * (x - 0.5f); }});
    g_routing.knobs[5].push_back({crusher, CrusherModule::RATE, [](float x) { return x > 0.5f ? 1.0f : 0.5f + x; }});

    // Mirror knob routes onto MIDI CCs so external controllers can drive the same mappings.
    // CC 14..19 mirror knob 0..5 by default.
    for (int knob = 0; knob < knobCount; ++knob) {
        const int cc = kMidiKnobMirrorBaseCC + knob;
        if (cc < 0 || cc >= kMidiCCCount) continue;
        g_routing.midiCC[cc] = g_routing.knobs[knob];
    }
    g_routing.midiCC[20].push_back({distortion, DistortionModule::GAIN});

    int altSwitchID         = g_hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Alternate);
    int bypassSwitchID      = g_hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Bypass);

    // Alternate footswitch: looper pressed & held
    g_routing.switches[bypassSwitchID].push_back({g_effects.micro_looper, bypassSwitchID, SwitchAction::Pressed});
    g_routing.switches[bypassSwitchID].push_back({g_effects.micro_looper, bypassSwitchID, SwitchAction::DoubleTapped});
    g_routing.switches[bypassSwitchID].push_back({g_effects.micro_looper, bypassSwitchID, SwitchAction::Held1s});
    g_routing.switches[altSwitchID].push_back({g_effects.micro_looper, altSwitchID, SwitchAction::Pressed});
    g_routing.switches[altSwitchID].push_back({g_effects.micro_looper, altSwitchID, SwitchAction::DoubleTapped});
    g_routing.switches[altSwitchID].push_back({g_effects.micro_looper, altSwitchID, SwitchAction::Held1s});
    g_routing.switches[altSwitchID].push_back({g_effects.reverb, altSwitchID, SwitchAction::Pressed});
    g_routing.switches[altSwitchID].push_back({g_effects.reverb, altSwitchID, SwitchAction::DoubleTapped});

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

            g_hardware.seed.PrintLine("CPU avg: %d%%  min: %d%%  max: %d%%", avg, minv, maxv);
            g_hardware.seed.PrintLine("sizeof MicroLooperModule: %d", sizeof(MicroLooperModule));
            // To read in terminal: screen /dev/tty.usbmodem395C326C34321 115200
            //g_hardware.seed.PrintLine("tick %d%%  odd: %d%%", g_midi.clock.tickCount, g_midi.beatLightOn);
        }

        // Run polling action.
        bool res = false;
        for (auto* effect : g_effects.chain) {
            if (!effect) continue;
            res |= effect->Poll();
        }

        if (g_effects.micro_looper) {
            g_effects.micro_looper->SetMidiClockRunning(g_midi.clock.running);
        }

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
            bool isDoubleTapPress = false;

            if (switchPressed && g_switches.enabledCache[sw] && g_switches.timeTilIdle[sw] > 0.0f) {
                isDoubleTapPress = true;
                g_switches.doubleEnabledCache[sw] = true;
            }

            // Dispatch all routes for this switch //TODO: add safety in case r.effect is nullptr but keep it possible for PostPreFX select.
            for (const auto &r : g_routing.switches[sw]) {
                switch (r.action) {
                    // SHORT PRESS -> fire on RisingEdge
                    case SwitchAction::Pressed:
                        if (switchPressed) {
                            // Keep existing behavior: for special switches, a double-tap
                            // should not also trigger the single-press callback on tap #2.
                            const bool suppressSingleForDoubleTap =
                                isDoubleTapPress &&
                                (r.switchId == altSwitchID || r.switchId == bypassSwitchID);

                            if (suppressSingleForDoubleTap) {
                                break;
                            }

                            if (r.switchId == altSwitchID) {
                                r.effect->AlternateFootswitchPressed();
                            } else if (r.switchId == bypassSwitchID) {
                                r.effect->BypassFootswitchPressed();
                            } else {
                                r.effect->FootswitchPressed(r.switchId);
                            }
                        }
                        break;

                    // DOUBLE TAP -> fire on RisingEdge of second tap (within tap window)
                    case SwitchAction::DoubleTapped:
                        if (switchPressed && isDoubleTapPress) {
                            if (r.switchId == altSwitchID) {
                                r.effect->AlternateFootswitchDoubleTapped();
                            } else if (r.switchId == bypassSwitchID) {
                                r.effect->BypassFootswitchDoubleTapped();
                            } else {
                                r.effect->FootswitchDoubleTapped(r.switchId);
                            }
                        }
                        break;

                    // RELEASE -> fire on FallingEdge (also clear held-guard)
                    case SwitchAction::Released:
                        if (switchReleased) {
                            if (r.switchId == altSwitchID) {
                                r.effect->AlternateFootswitchReleased();
                            } else if (r.switchId == bypassSwitchID) {
                                r.effect->BypassFootswitchReleased();
                            } else {
                                r.effect->FootswitchReleased(r.switchId);
                            }
                            // Reset held flag so future holds can fire
                            g_switches.heldFired[sw] = false;
                        }
                        break;

                    // HELD (1s) -> fire once when hold threshold reached, guarded by switchesHeldFired
                    case SwitchAction::Held1s:
                        if (switchHeld && !g_switches.heldFired[sw]) {
                            if (r.switchId == altSwitchID) {
                                r.effect->AlternateFootswitchHeldFor1Second();
                            } else if (r.switchId == bypassSwitchID) {
                                r.effect->BypassFootswitchHeldFor1Second();
                            } else {
                                r.effect->FootswitchHeldFor1Second(r.switchId);
                            }
                            g_switches.heldFired[sw] = true; // prevent repeated calls until release
                        }
                        break;
                }
            }

            // Ensure the held-flag is cleared if user releases the button without any 'Released' route mapped
            // (keeps held-guard consistent even if no route calls reset it)
            if (switchReleased) {g_switches.heldFired[sw] = false;}

            // Footswitch 2: Wet/Dry toggle
            // CH1_LEVEL = channel 0 = wet (effect chain output)
            // CH2_LEVEL = channel 1 = dry (original input)
            switch (sw) {
                case 2:
                    if (switchPressed) {
                        // Wet mode: wet signal only, mute dry
                        g_effects.mixer->SetParameterAsFloat(MixerModule::CH1_LEVEL, 1.0f);  // wet on
                        g_effects.mixer->SetParameterAsFloat(MixerModule::CH2_LEVEL, 0.0f);  // dry off
                        g_effects.micro_looper->SetParameterAsFloat(MicroLooperModule::IN_MIX, 1.0f);
                    }
                    if (switchReleased) {
                        // Dry mode: both signals at full, looper MIX = 0.0
                        g_effects.mixer->SetParameterAsFloat(MixerModule::CH1_LEVEL, 1.0f);
                        g_effects.mixer->SetParameterAsFloat(MixerModule::CH2_LEVEL, 1.0f);
                        g_effects.micro_looper->SetParameterAsFloat(MicroLooperModule::IN_MIX, 0.0f);
                    }
                    break;
                case 4:
                    if (switchPressed) {
                        g_effects.micro_looper->SetParameterAsBool(MicroLooperModule::PITCH_DIRECTION, false);
                    }
                    if (switchReleased) {
                        g_effects.micro_looper->SetParameterAsBool(MicroLooperModule::PITCH_DIRECTION, true);
                    }
                    break;
                case 5:
                    if (switchPressed) {
                        g_effects.micro_looper->SetParameterAsBool(MicroLooperModule::SPEED_ERROR, true);
                    }
                    if (switchReleased) {
                        g_effects.micro_looper->SetParameterAsBool(MicroLooperModule::SPEED_ERROR, false);
                    }
                    break;
                case 6:
                    if (switchPressed) {
                        g_effects.micro_looper->SetParameterAsBinnedValue(MicroLooperModule::LOOP_MODE, MicroLooperModule::SAMPLER);
                    }
                    if (switchReleased) {
                        g_effects.micro_looper->SetParameterAsBinnedValue(MicroLooperModule::LOOP_MODE, MicroLooperModule::OVERDUB);
                    }
                    break;
                case 7:
                    if (switchPressed) {
                        g_effects.micro_looper->SetEnabled(false);
                        g_effects.reverb->SetEnabled(true);
                        g_effects.pitchshifter->SetEnabled(true);
                    }
                    if (switchReleased) {
                        g_effects.micro_looper->SetEnabled(true);
                        g_effects.reverb->SetEnabled(false);
                        g_effects.pitchshifter->SetEnabled(false);
                    }
                    break;
            }

            if (g_switches.enabledCache[sw] == true) {
                g_switches.timeTilIdle[sw] -= elapsedTimeInSeconds;

                if (g_switches.timeTilIdle[sw] <= 0) {
                    g_switches.enabledCache[sw] = false;
                    g_switches.timeTilIdle[sw] = 0.0f;
                    g_switches.doubleEnabledCache[sw] = false;
                }
            }

            if (switchPressed) {
                if (isDoubleTapPress) {
                    // Consume the double tap so the next press starts a fresh tap window.
                    g_switches.enabledCache[sw] = false;
                    g_switches.timeTilIdle[sw] = 0.0f;
                    g_switches.doubleEnabledCache[sw] = false;
                } else {
                    // Start (or restart) the single-tap window.
                    g_switches.enabledCache[sw] = true;
                    g_switches.timeTilIdle[sw] = g_switches.idleTimeInSeconds;
                }
            }
        }

        if (g_knobs.initialized) {
            // Only iterate the real knobs reported by hardware
            for (int k = 0; k < knobCount; ++k) {
                if (!g_knobs.cacheChanged[k]) continue;

                float v = g_knobs.cache[k]; // normalized 0..1 after deadzone mapping

                // Send to all mapped targets of knob k
                DispatchParamRoutes(g_routing.knobs[k], v);
            }
        }
    }
}
