#include "looper_module.h"

using namespace bkshepherd;

float DSY_SDRAM_BSS LooperModule::buffer_[kNumLayers][kMaxBufferSize];

static const int s_paramCount = 4;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Layer",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.0f},
        knobMapping : -1,
        midiCCMapping : 14
    },
    {
        name : "Fading",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.0f},
        knobMapping : -1,
        midiCCMapping : 15
    },
    {
        name : "Speed",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : -1,
        midiCCMapping : 15
    },
    {
        name : "Slice",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : -1,
        midiCCMapping : 15
    },
};

// Default Constructor
LooperModule::LooperModule()
    : BaseEffectModule() {
    // Set the name of the effect
    m_name = "Looper";

    // Setup the meta data reference for this Effect
    m_paramMetaData = s_metaData;

    // Initialize Parameters for this Effect
    this->InitParams(s_paramCount);

    ResetBuffer();
}

// Destructor
LooperModule::~LooperModule() {
    // No Code Needed
}

void LooperModule::ResetBuffer() {
    is_playing_         = false;
    is_recording_       = false;
    first_layer_        = true;
    loop_length_        = 0;
    loop_length_f_      = 0.0f;
    mod                 = kMaxBufferSize;

    modifier_on_        = false;

    playing_head_.Reset();
    recording_head_.Reset();

    std::fill(&buffer_[0][0], &buffer_[0][0] + kNumLayers * kMaxBufferSize, 0.0f);
    n_recorded_layers_ = 0;
}

void LooperModule::ClearTopLayers(size_t clear_from) {
    for (int layer = clear_from; layer < n_recorded_layers_; ++layer) {
        std::fill(&buffer_[layer][0], &buffer_[layer][0] + kMaxBufferSize, 0.0f);
    }
    n_recorded_layers_ = std::min(n_recorded_layers_, clear_from);
};

void LooperModule::SquashLayers() {
    for (size_t sample = 0; sample < mod; ++sample) {
        buffer_[0][sample] += buffer_[1][sample];
    }

    for (size_t layer = 1; layer < (kNumLayers - 1); ++layer) {
        for (size_t sample = 0; sample < mod; ++sample) {
            buffer_[layer][sample] = buffer_[layer + 1][sample];
        }
    }

    std::fill(&buffer_[kNumLayers - 1][0], &buffer_[kNumLayers - 1][0] + kMaxBufferSize, 0.0f);
}

void LooperModule::AlternateFootswitchPressed(){
    modifier_on_ = !modifier_on_;
    if (!modifier_on_) {
        offset_ = 0.0f;
        playing_head_.SyncTo(recording_head_);
    } else {
        offset_ = playing_head_.GetHeadPosition();
    }
    
}

void LooperModule::BypassFootswitchPressed(){
    if (!is_recording_) {
        if (n_recorded_layers_ == 0) {
            recording_layer_ = 0;
        } else {
            float layer_knob_val = GetParameterAsFloat(0);
            if (std::abs(prev_layer_knob_val_ - layer_knob_val) > 0.05f) {
                prev_layer_knob_val_ = layer_knob_val;
                selected_layer_ = static_cast<size_t>(std::min(layer_knob_val, 0.99f) * n_recorded_layers_);
            }
            size_t target_layer = std::min<size_t>(selected_layer_ + 1, kNumLayers - 1);
            ClearTopLayers(target_layer);
            recording_layer_ = target_layer;
        }

        is_recording_ = true;
        is_playing_ = true;
        prev_wrap_around_count_ = recording_head_.GetWrapAroundCount();
    } else {
        n_recorded_layers_ = std::min(static_cast<uint8_t>(recording_layer_ + 1), static_cast<uint8_t>(kNumLayers));
        if (recording_layer_ == (kNumLayers - 1)) {
            SquashLayers();
        }

        is_recording_ = false;
        is_playing_ = true;
        recording_layer_ = 0;

        if (first_layer_) {
            selected_layer_ = 0;
            first_layer_ = false;
            mod = loop_length_;
            loop_length_ = 0;
        } else {
            selected_layer_++;
        }
    }
};

void LooperModule::AlternateFootswitchHeldFor1Second() {
    ResetBuffer();
};

void LooperModule::WriteBuffer(float in)
{
    float recording_head_position_f = recording_head_.GetHeadPosition();
    float playing_head_position_f = playing_head_.GetHeadPosition();
    size_t recording_index = static_cast<size_t>(recording_head_position_f);
    size_t playing_index = static_cast<size_t>(playing_head_position_f);

    buffer_[recording_layer_][recording_index] +=  in;

    if (first_layer_) {
        loop_length_++;
    }
};

void LooperModule::ProcessStereo(float inL, float inR) {
    float fading = 1.0f - GetParameterAsFloat(1);
    float speed = 1.0f;
    float slice = 1.0f;
    
    if (modifier_on_) {
        speed = 4.0f * (GetParameterAsFloat(2) - 0.5f);
        slice = std::max(0.01f, GetParameterAsFloat(3));
    }

    //automatic looptime
    /*if(loop_length_ >= kMaxBufferSize)
    {
        first_layer_ = false;
        mod = kMaxBufferSize;
        loop_length_   = 0;
    }*/

    /*if (one_pass_recording_) {
        uint16_t curr_wrap_around_count = recording_head_.GetWrapAroundCount();
        if (curr_wrap_around_count != prev_wrap_around_count_) {
            StopRecording();
        }
        prev_wrap_around_count_ = curr_wrap_around_count;
    }*/

    if (is_playing_) {
        playing_head_.SetSpeed(speed);
        playing_head_.UpdatePosition(first_layer_, mod, slice, offset_);
        recording_head_.UpdatePosition(first_layer_, mod);
    }

    float playing_head_position_f = playing_head_.GetHeadPosition();
    size_t playing_head_position = static_cast<size_t>(playing_head_position_f);

    float recording_head_position_f = recording_head_.GetHeadPosition();
    size_t recording_head_position = static_cast<size_t>(recording_head_position_f);

    m_audioLeft = 0.0f;

    float layer_knob_val = GetParameterAsFloat(0);
    if (std::abs(layer_knob_val - prev_layer_knob_val_) > 0.05f) {
        prev_layer_knob_val_ = layer_knob_val;
        selected_layer_ = static_cast<size_t>(layer_knob_val * n_recorded_layers_);
    }
    
    for(size_t l = 0; l <= (is_recording_ ? recording_layer_ : selected_layer_); ++l) {
        m_audioLeft += buffer_[l][playing_head_position];
    }

    if (is_recording_) {
        for(int l = 0; l <= recording_layer_; ++l) {
            buffer_[l][recording_head_position] = buffer_[l][recording_head_position] * fading;
        }
    }
    
    m_audioLeft = m_audioLeft + inL;

    if (is_recording_) {
        WriteBuffer(inL);
    }
}

float LooperModule::GetBrightnessForLED(int led_id) const {
    float led_brightness;
    
    if (led_id == 0) {
        led_brightness = is_recording_ ? 1.0f : 0.0f;
    } else {
        led_brightness = modifier_on_ ? 1.0f : 0.0f;
    }
    return led_brightness;
}