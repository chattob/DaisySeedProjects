#pragma once
#ifndef PITCH_SHIFTER_MODULE_H
#define PITCH_SHIFTER_MODULE_H

#include <stdint.h>

#include "base_effect_module.h"
#ifdef __cplusplus

/** @file pitch_shifter_module.h */

namespace bkshepherd {

class PitchShifterModule : public BaseEffectModule {
  public:
    PitchShifterModule();
    ~PitchShifterModule();

    enum Param {
        SEMITONE = 0,
        CROSSFADE,
        DIRECTION,
        MODE,
        SHIFT,
        RETURN,
        SMOOTH,
        PARAM_COUNT
    };

    void Init(float sample_rate) override;
    void ProcessMono(float in) override;
    void ProcessStereo(float inL, float inR) override;
    void ParameterChanged(int parameter_id) override;
    void SetParameterAsMagnitude(int parameter_id, float value) override;

    /*bool AlternateFootswitchForTempo() const override { return false; }
    void AlternateFootswitchPressed() override;
    void AlternateFootswitchReleased() override;*/
    void DrawUI(OneBitGraphicsDisplay &display, int currentIndex, int numItemsTotal, Rectangle boundsToDrawIn,
                bool isEditing) override;

  private:
    void SetTranspose(float semitone);
    float ProcessMomentaryMode(float in);
    void ProcessSemitoneTargetChange();

    bool m_latching = true;
    bool m_directionDown = true;
    bool m_alternateFootswitchPressed = false;
    bool m_smoothSemitone = false;
    bool m_semitoneFromMagnitude = false;

    float m_semitoneTarget = 0;
    float m_semitoneContinuous = 0;

    uint32_t m_sampleCounter = 0;
    uint32_t m_samplesToDelayShift = 0;
    uint32_t m_samplesToDelayReturn = 0;

    bool m_transitioningShift = false;
    bool m_transitioningReturn = false;

    float m_percentageTransitionComplete = 0.0;
};
} // namespace bkshepherd
#endif
#endif
