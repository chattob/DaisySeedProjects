#include "daisysp.h"
#include "daisy_seed.h"
#include <string.h>
#include "Hardware-Modules/guitar_pedal_125b.h"
#include "Effect-Modules/base_effect_module.h"
#include "Effect-Modules/delay_module.h"
#include "Effect-Modules/looper_module.h"
#include "Effect-Modules/distortion_module.h"
#include "Effect-Modules/pitch_shifter_module.h"
#include "Util/audio_utilities.h"
#include <vector>

using namespace daisy;
using namespace daisysp;
using namespace bkshepherd;

GuitarPedal125B hardware;
constexpr size_t kBlockSize = 48;

// Effect Related Variables
std::vector<BaseEffectModule*> effectChain;

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

bool isCrossFading = false;
bool isCrossFadingForward = true; // True goes Source->Target, False goes Target->Source
CrossFade crossFaderLeft, crossFaderRight;
float crossFaderTransitionTimeInSeconds = 0.1f;
int crossFaderTransitionTimeInSamples;
int samplesTilCrossFadingComplete;
CpuLoadMeter cpuLoadMeter;

enum class KnobCurve {
    LinearInc,
    LinearDec,
};

struct KnobRoute {
    BaseEffectModule*   effect;
    int                 paramId;   // index into that effect's parameter array
    float               outMin     = 0.0f;   // mapped range min
    float               outMax     = 1.0f;   // mapped range max
};

std::vector<std::vector<KnobRoute>> knobRoutes;

enum class SwitchAction {
    AltPressed,
    AltReleased,
    AltHeld1s,

    BypassPressed,
    BypassReleased,
    BypassHeld1s,
};

struct SwitchRoute {
    BaseEffectModule* effect;
    SwitchAction      action;
};

std::vector<std::vector<SwitchRoute>> switchRoutes;

static inline BaseEffectModule* First()
{
    return effectChain.empty() ? nullptr : effectChain[0];
}

//======================================================================
//                            AUDIO CALLBACK
//======================================================================
static void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    cpuLoadMeter.OnBlockStart();

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
    if (auto* effect = First()) {
        led1Brightness = effect->GetBrightnessForLED(0);
        led2Brightness = effect->GetBrightnessForLED(1);
    } 

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

    auto* looper = new LooperModule();
    auto* delay  = new DelayModule();
    auto* distortion  = new DistortionModule();
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
    distortion->SetParameterAsBinnedValue(DistortionModule::DIST_TYPE, 5);

    looper->SetEnabled(true);
    delay->SetEnabled(false);
    distortion->SetEnabled(false);
    pitch_shifter->SetEnabled(false);

    effectChain.push_back(looper);
    effectChain.push_back(delay);
    //effectChain.push_back(distortion);
    //effectChain.push_back(pitch_shifter);
    
    for (auto* effect : effectChain) {
        effect->Init(sample_rate);
    }

    // Size the routes to the real knob count
    const int knobCount = hardware.GetParameterControlCount();
    knobRoutes.resize(knobCount);

    // Size the routes to the real switches count
    switchRoutes.resize(hardware.GetSwitchCount());

    // Setup knob routes
    knobRoutes[0].push_back({looper, LooperModule::LAYER});

    knobRoutes[1].push_back({looper, LooperModule::SPEED});

    knobRoutes[2].push_back({looper, LooperModule::SLICE});

    knobRoutes[3].push_back({delay, DelayModule::MOD_AMPLITUDE});

    knobRoutes[4].push_back({distortion, DistortionModule::GAIN, 0.0f, 0.8f});
    knobRoutes[4].push_back({distortion, DistortionModule::INTENSITY, 0.0f, 0.8f});
    knobRoutes[4].push_back({distortion, DistortionModule::MIX, 0.0f, 0.8f});
    knobRoutes[4].push_back({distortion, DistortionModule::LEVEL, 1.0f, 0.15f});
    knobRoutes[4].push_back({distortion, DistortionModule::TONE, 0.5f, 0.4f});

    /*knobRoutes[0].push_back({delay, DelayModule::DELAY_MIX});
    knobRoutes[1].push_back({delay, DelayModule::DELAY_TIME});
    knobRoutes[2].push_back({delay, DelayModule::D_FEEDBACK});
    knobRoutes[3].push_back({delay, DelayModule::MOD_AMPLITUDE});
    knobRoutes[4].push_back({delay, DelayModule::MOD_FREQ});*/

    /*knobRoutes[0].push_back({distortion, DistortionModule::LEVEL});
    knobRoutes[1].push_back({distortion, DistortionModule::GAIN});
    knobRoutes[2].push_back({distortion, DistortionModule::TONE});
    knobRoutes[3].push_back({distortion, DistortionModule::DIST_TYPE});
    knobRoutes[4].push_back({distortion, DistortionModule::INTENSITY});
    knobRoutes[5].push_back({distortion, DistortionModule::OVERSAMP});*/
    
    /*knobRoutes[0].push_back({pitch_shifter, PitchShifterModule::SEMITONE});
    knobRoutes[1].push_back({pitch_shifter, PitchShifterModule::CROSSFADE});
    knobRoutes[2].push_back({pitch_shifter, PitchShifterModule::DIRECTION});
    knobRoutes[3].push_back({pitch_shifter, PitchShifterModule::MODE});
    knobRoutes[4].push_back({pitch_shifter, PitchShifterModule::SHIFT});
    knobRoutes[5].push_back({pitch_shifter, PitchShifterModule::RETURN});*/

    // 1: layers, 2: fading, 3: stability/bitcrusher, 4: slice/stretch, 5: speed/pitch, 6: distortion
    // A: single/all/direct B:fixed/flex C:

    int altSwitchID    = hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Alternate);
    int bypassSwitchID = hardware.GetPreferredSwitchIDForSpecialFunctionType(SpecialFunctionType::Bypass);

    // Alternate footswitch: toggle delay pressed & looper held
    switchRoutes[altSwitchID].push_back({looper, SwitchAction::AltPressed});
    switchRoutes[altSwitchID].push_back({looper, SwitchAction::AltHeld1s});
    switchRoutes[altSwitchID].push_back({delay, SwitchAction::BypassPressed});
    switchRoutes[altSwitchID].push_back({distortion, SwitchAction::BypassPressed});
    switchRoutes[altSwitchID].push_back({pitch_shifter, SwitchAction::BypassPressed});

    // Main/bypass footswitch
    switchRoutes[bypassSwitchID].push_back({looper, SwitchAction::BypassPressed});

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
    // hardware.seed.StartLog();

    while (1) {
        // Handle Clock Time
        uint32_t currentTimeStampUS = System::GetUs();
        volatile bool frozen;
        if (currentTimeStampUS == lastTimeStampUS) {
            frozen = true;
        }
        uint32_t elapsedTimeStampUS = currentTimeStampUS - lastTimeStampUS;
        lastTimeStampUS = currentTimeStampUS;
        float elapsedTimeInSeconds = (elapsedTimeStampUS / 1000000.0f);
        secondsSinceStartup = secondsSinceStartup + elapsedTimeInSeconds;

        // Run polling action.
        bool res = false;
        for (auto* effect : effectChain) {
            if (!effect) continue;
            res |= effect->Poll();
        }

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

            // Dispatch all routes for this switch
            for (const auto &r : switchRoutes[sw]) {
                if (!r.effect) continue;

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

                    r.effect->SetParameterAsMagnitude(r.paramId, r.outMin + v * (r.outMax - r.outMin));
                }
            }
        }

    }
}
