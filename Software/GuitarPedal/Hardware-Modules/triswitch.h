#pragma once
#include "daisy_seed.h"

using namespace daisy;

struct TriPin {
    Pin a;
    Pin b;
};

struct TriSwitch {
    Switch a, b; // libDaisy

    void Init(const TriPin& pin, float ctrl_hz=1000.0f){
        a.Init(pin.a, ctrl_hz, Switch::TYPE_TOGGLE, Switch::POLARITY_INVERTED);
        b.Init(pin.b, ctrl_hz, Switch::TYPE_TOGGLE, Switch::POLARITY_INVERTED);
    }

    void Debounce(){
        a.Debounce();
        b.Debounce(); 
    }

    float Value() const {
        const bool al=a.Pressed(), bl=b.Pressed();
        if(al && !bl)  return 0.0f;  // left
        if(!al && !bl) return 0.5f;  // middle
        if(!al && bl)  return 1.0f;  // right
        return 0.5f;                 // both low -> treat as middle
    }
};