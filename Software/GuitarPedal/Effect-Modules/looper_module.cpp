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
    /*is_playing_         = false;
    is_recording_       = false;
    first_layer_        = true;
    loop_length_        = 0;
    loop_length_f_      = 0.0f;
    mod                 = kMaxBufferSize;

    modifier_on_        = false;

    playing_head_.Reset();
    recording_head_.Reset();

    std::fill(&buffer_[0][0], &buffer_[0][0] + kNumLayers * kMaxBufferSize, 0.0f);
    n_recorded_layers_ = 0;*/
    is_playing_           = false;
    is_recording_         = false;
    first_layer_          = true;
    loop_length_          = 0;
    mod                   = kMaxBufferSize;

    modifier_on_          = false;
    offset_               = 0.0f;
    recording_layer_      = 0;
    selected_layer_       = 0;
    n_recorded_layers_    = 0;
    prev_layer_knob_val_  = 0.0f;
    prev_wrap_around_count_ = 0;

    playing_head_.Reset();
    recording_head_.Reset();
}


/*void LooperModule::ClearTopLayers(size_t clear_from) {
    for (int layer = clear_from; layer < n_recorded_layers_; ++layer) {
        std::fill(&buffer_[layer][0], &buffer_[layer][0] + kMaxBufferSize, 0.0f);
    }
    n_recorded_layers_ = std::min(n_recorded_layers_, clear_from);
};*/
void LooperModule::ClearTopLayers(size_t clear_from)
{
    // 1) Logically drop all layers above clear_from
    if(clear_from < n_recorded_layers_)
    {
        n_recorded_layers_ = clear_from;
    }

    // 2) Physically clear ONLY this layer so it’s clean for reuse
    if(clear_from < kNumLayers)
    {
        std::fill(&buffer_[clear_from][0],
                  &buffer_[clear_from][0] + kMaxBufferSize,
                  0.0f);
    }
}

void LooperModule::SquashLayers() {
    /*for (size_t sample = 0; sample < mod; ++sample) {
        buffer_[0][sample] += buffer_[1][sample];
    }

    for (size_t layer = 1; layer < (kNumLayers - 1); ++layer) {
        for (size_t sample = 0; sample < mod; ++sample) {
            buffer_[layer][sample] = buffer_[layer + 1][sample];
        }
    }

    std::fill(&buffer_[kNumLayers - 1][0], &buffer_[kNumLayers - 1][0] + kMaxBufferSize, 0.0f);*/
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
            selected_layer_ = std::min<size_t>(selected_layer_ + 1, kNumLayers - 1);
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

float LooperModule::GetBrightnessForLED(int led_id) const
{
    // Common blink step timing (ms)
    const uint32_t BLINK_INTERVAL_MS = 80;

    // One timestamp per call
    const uint32_t now_ms = daisy::System::GetNow();

    // --------------------------------------------------------------------
    // 1. BASE BRIGHTNESS PER LED
    // --------------------------------------------------------------------
    float base = 0.0f;

    if (led_id == 0)
    {
        // LED 0: recording indicator with fade in last 20% of loop
        if (is_recording_ && mod > 0)
        {
            float pos   = playing_head_.GetHeadPosition();     // [0, mod)
            float phase = pos / static_cast<float>(mod);       // [0, 1)

            if (phase <= 0.8f)
            {
                // First 80% of loop: fully on
                base = 1.0f;
            }
            else
            {
                // Last 20%: fade 1.0 -> 0.0
                float t = (phase - 0.8f) / 0.2f;               // [0,1]
                if (t > 1.0f)
                    t = 1.0f;
                base = 1.0f - t;
            }
        }
        else
        {
            // Not recording: base off (pattern may override later)
            base = 0.0f;
        }
    }
    else
    {
        // LED 1: modifier on/off base
        base = modifier_on_ ? 1.0f : 0.0f;
    }

    // --------------------------------------------------------------------
    // 2. TRIGGER PATTERN ON LOOP WRAP  (LED 0, ONLY WHEN NOT RECORDING)
    // --------------------------------------------------------------------
    if (led_id == 0)
    {
        if (is_recording_)
        {
            // While recording: no pattern for LED 0
            blink_[0]              = false;
            blink_tracker_[0]      = 0;
            pattern_brightness_[0] = 0.0f;
        }
        else
        {
            uint16_t curr_wrap_around_count = recording_head_.GetWrapAroundCount();
            if (curr_wrap_around_count != prev_wrap_around_count_)
            {
                prev_wrap_around_count_ = curr_wrap_around_count;

                blink_[0]              = false;
                blink_tracker_[0]      = 0;
                pattern_brightness_[0] = 0.0f;

                // Clear old pattern
                for (int i = 0; i < kMaxBlinkSteps; ++i)
                    blink_pattern_[0][i] = -1;

                // Short blink  = [1,0]
                // Long blink   = [1,1,1,0]
                //
                // Layer 1 (selected_layer_ = 0):   •
                // Layer 2 (1):                     • •
                // Layer 3 (2):                     • • •
                // Layer 4 (3):                     —
                // Layer 5 (4):                     — •
                // Layer 6 (5):                     — • •
                // Layer 7 (6):                     — • • •

                int idx = 0;
                switch (selected_layer_)
                {
                case 0: // layer 1
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0;
                    break;

                case 1: // layer 2
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0;
                    break;

                case 2: // layer 3
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0;
                    break;

                case 3: // layer 4 (long): 1,1,1,0
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0;
                    break;

                case 4: // layer 5: long + short
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0; // long
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0; // short
                    break;

                case 5: // layer 6: long + 2 short
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0; // long
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0; // short
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0; // short
                    break;

                case 6: // layer 7: long + 3 short
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0; // long
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0; // short
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0; // short
                    blink_pattern_[0][idx++] = 1;
                    blink_pattern_[0][idx++] = 0; // short
                    break;

                default:
                    break;
                }

                if (idx > 0)
                {
                    if (idx < kMaxBlinkSteps)
                        blink_pattern_[0][idx] = -1; // terminator

                    blink_[0]              = true;
                    blink_tracker_[0]      = 0;
                    pattern_brightness_[0] = (blink_pattern_[0][0] > 0) ? 1.0f : 0.0f;
                    next_toggle_time_[0]   = now_ms + BLINK_INTERVAL_MS;
                }
            }
        }
    }

    // --------------------------------------------------------------------
    // 3. TRIGGER PATTERN ON SELECTED LAYER CHANGE  (LED 1)
    // --------------------------------------------------------------------
    if (led_id == 1)
    {
        if (selected_layer_ != prev_selected_layer_)
        {
            prev_selected_layer_ = selected_layer_;

            blink_[1]              = false;
            blink_tracker_[1]      = 0;
            pattern_brightness_[1] = 0.0f;

            // single short blink: [1,0]
            blink_pattern_[1][0] = 1;
            blink_pattern_[1][1] = 0;
            blink_pattern_[1][2] = -1;
            for (int i = 3; i < kMaxBlinkSteps; ++i)
                blink_pattern_[1][i] = -1;

            blink_[1]              = true;
            blink_tracker_[1]      = 0;
            pattern_brightness_[1] = 1.0f;
            next_toggle_time_[1]   = now_ms + BLINK_INTERVAL_MS;
        }
    }

    // --------------------------------------------------------------------
    // 4. ADVANCE BLINK PATTERN FOR THIS LED (TIME-BASED)
    // --------------------------------------------------------------------
    if (blink_[led_id])
    {
        if (now_ms >= next_toggle_time_[led_id])
        {
            next_toggle_time_[led_id] = now_ms + BLINK_INTERVAL_MS;

            blink_tracker_[led_id]++;

            if (blink_tracker_[led_id] >= kMaxBlinkSteps)
            {
                blink_[led_id]              = false;
                blink_tracker_[led_id]      = 0;
                pattern_brightness_[led_id] = 0.0f;
            }
            else
            {
                int8_t step = blink_pattern_[led_id][blink_tracker_[led_id]];
                if (step >= 0)
                {
                    pattern_brightness_[led_id] = (step > 0) ? 1.0f : 0.0f;
                }
                else
                {
                    blink_[led_id]              = false;
                    blink_tracker_[led_id]      = 0;
                    pattern_brightness_[led_id] = 0.0f;
                }
            }
        }
    }

    // --------------------------------------------------------------------
    // 5. FINAL BRIGHTNESS
    // --------------------------------------------------------------------
    // If a blink pattern is active, it overrides the base completely.
    if (blink_[led_id])
        return pattern_brightness_[led_id];
    else
        return base;
}
