#include "effect_router_module.h"

using namespace bkshepherd;

EffectRouterModule::EffectRouterModule() : BaseEffectModule() {}

EffectRouterModule::~EffectRouterModule() {}

void EffectRouterModule::Init(float sample_rate) {
    // Router itself does not do much on init.
    sample_rate_ = sample_rate;
    // Make sure router is always “enabled” so the main loop will call it.
    SetEnabled(true);
}

void EffectRouterModule::SetInner(BaseEffectModule* inner)
{
    inner_ = inner;
}

// Called from switchRoutes when triswitch_0_left goes to ON
void EffectRouterModule::AlternateFootswitchPressed() {
    tri_left_active_ = true;
}

// Called from switchRoutes when triswitch_0_left leaves ON
void EffectRouterModule::AlternateFootswitchReleased() {
    tri_left_active_ = false;
}

bool EffectRouterModule::Poll() {
    // Forward Poll to the inner module, so its internal logic still runs
    bool changed = false;
    if(inner_)
        changed |= inner_->Poll();
    return changed;
}

void EffectRouterModule::ProcessStereoBlock(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    if(!inner_) {
        CopyStereoBlock(in, out, size);
        return;
    }

    // Condition 1: tri switch must be in "left" position
    if(!tri_left_active_) {
        CopyStereoBlock(in, out, size);
        return;
    }

    // Condition 2: effect must be enabled
    if(!inner_->IsEnabled()) {
        CopyStereoBlock(in, out, size);
        return;
    }

    // Both conditions satisfied: actually process through pitch shifter
    inner_->ProcessStereoBlock(in, out, size);
}
