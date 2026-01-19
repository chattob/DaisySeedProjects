#include "micro_looper_module.h"
#include "../Util/shy_fft.h"
#include <cmath>
#include <cstring>

using namespace bkshepherd;

float DSY_SDRAM_BSS MicroLooperModule::buffer_[kMicroLoopMaxSize];
float DSY_SDRAM_BSS MicroLooperModule::stretched_buffer_[kMicroLoopMaxStretchedSize];

// State machine for offline stretching
enum class StretchState {
    IDLE,
    GATHER_FRAME,
    APPLY_ANALYSIS_WINDOW,
    DO_FFT,
    EXTRACT_MAGNITUDES,
    RANDOMIZE_PHASES,
    DO_IFFT,
    APPLY_SYNTHESIS_WINDOW,
    ADD_TO_OUTPUT,
    CHECK_MORE_SYNTH,
    ADVANCE_READ,
    DONE
};

// RNG for phase randomization
struct XorShift32 {
    uint32_t state = 0x12345678u;

    inline uint32_t nextU32() {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    inline float randSigned() {
        return ((nextU32() >> 8) * (1.0f / 8388608.0f)) - 1.0f;
    }
};

// ============================================================
// STATIC BUFFERS - SDRAM for large ones
// ============================================================
static ShyFFT<float, N> s_fft;

// Working buffers - all in SDRAM
static float DSY_SDRAM_BSS s_frame_time[N];
static float DSY_SDRAM_BSS s_frame_freq[N];
static float DSY_SDRAM_BSS s_magnitudes[N/2];

// Normalization buffer for offline OLA
static float DSY_SDRAM_BSS s_stretch_norm[kMicroLoopMaxStretchedSize];

// Window - in SDRAM
static float DSY_SDRAM_BSS s_win[N];
static float DSY_SDRAM_BSS s_win2[N];

// Processing state
static volatile StretchState s_stretch_state = StretchState::IDLE;
static size_t s_synth_count = 0;

// Snapshot for processing
static float DSY_SDRAM_BSS s_snapshot_buffer[N];
static float DSY_SDRAM_BSS s_result_buffer[N];

static XorShift32 s_rng;

// ============================================================
// HELPER FUNCTIONS
// ============================================================
static void BuildHann(float* w, size_t n) {
    for(size_t i = 0; i < n; ++i)
        w[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * (float)i / (float)(n - 1)));
}

static inline void GatherFrameFromBuffer(float* dst, const float* buffer, size_t buffer_len,
                                         size_t start) {
    if(buffer_len == 0) {
        std::fill(dst, dst + N, 0.0f);
        return;
    }
    for(size_t i = 0; i < N; ++i)
        dst[i] = buffer[(start + i) % buffer_len];
}

static inline void OLA_AddFrame(float* out, float* norm, size_t out_size,
                                 size_t start, const float* x, const float* w2) {
    for(size_t i = 0; i < N; ++i) {
        size_t p = (start + i) % out_size;
        out[p] += x[i];
        norm[p] += w2[i];
    }
}

// ============================================================
// PARAMETER METADATA
// ============================================================
static const char *s_LoopModes[2] = {"Overdub", "Sampler"};

static const int s_paramCount = 3;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Loop mode",
        valueType : ParameterValueType::Binned,
        valueBinCount : 2,
        valueBinNames : s_LoopModes,
        defaultValue : {.uint_value = 0},
        knobMapping : 0,
        midiCCMapping : -1
    },
    {
        name : "Speed",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.75f},
        knobMapping : 0,
        midiCCMapping : -1
    },
    {
        name : "Loop mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.0f},
        knobMapping : 1,
        midiCCMapping : -1
    },
};

// Default Constructor
MicroLooperModule::MicroLooperModule()
: BaseEffectModule()
{
    m_name          = "Micro-looper";
    m_paramMetaData = s_metaData;
    InitParams(s_paramCount);
}

// Destructor
MicroLooperModule::~MicroLooperModule() {}

void MicroLooperModule::Init(float sample_rate)
{
    BaseEffectModule::Init(sample_rate);

    ResetBuffer();

    // Initialize FFT
    s_fft.Init();

    // Build windows
    BuildHann(s_win, N);
    for(size_t i = 0; i < N; ++i)
        s_win2[i] = s_win[i] * s_win[i];

    // Clear buffers
    std::memset(s_magnitudes, 0, sizeof(s_magnitudes));

    // Reset state
    s_stretch_state = StretchState::IDLE;
    s_synth_count = 0;
}

void MicroLooperModule::AlternateFootswitchPressed() {
    if (!is_recording_) {
        clock_beat_ = false;
        armed_recording_ = true;
    } else {
        clock_beat_ = false;
        armed_stop_ = true;
    }
}

void MicroLooperModule::ResetBuffer() {
    is_playing_         = false;
    is_recording_       = false;
    first_layer_        = true;
    loop_length_        = 0;
    mod_                = kMicroLoopMaxSize;
    is_stretching_      = false;
    use_stretched_buffer_ = false;
    stretched_length_   = 0;
    stretch_read_pos_   = 0;
    stretch_write_pos_  = 0;
    stretch_total_frames_ = 0;
    stretch_frames_done_ = 0;

    stretch_playing_head_.Reset();
    playing_head_.Reset();
    recording_head_.Reset();
    prev_wraparound_count_ = 0;

    std::fill(&buffer_[0], &buffer_[0] + kMicroLoopMaxSize, 0.0f);
    s_stretch_state = StretchState::IDLE;
    s_synth_count = 0;
}

void MicroLooperModule::WriteBuffer(float in)
{
    float recording_head_position_f = recording_head_.GetHeadPosition();
    size_t recording_index = static_cast<size_t>(recording_head_position_f);

    buffer_[recording_index] =  in;

    loop_length_++;
};

void MicroLooperModule::StartStretching()
{
    if(mod_ == 0) {
        is_stretching_ = false;
        use_stretched_buffer_ = false;
        stretched_length_ = 0;
        return;
    }

    stretch_read_pos_ = 0;
    stretch_write_pos_ = 0;
    stretch_frames_done_ = 0;
    stretch_total_frames_ = (mod_ + H_IN - 1) / H_IN;
    size_t total_synth_frames = stretch_total_frames_ * STRETCH;
    stretched_length_ = total_synth_frames * H_OUT;
    if(stretched_length_ == 0) {
        is_stretching_ = false;
        use_stretched_buffer_ = false;
        return;
    }
    if(stretched_length_ > kMicroLoopMaxStretchedSize) {
        stretched_length_ = kMicroLoopMaxStretchedSize;
    }

    std::fill(&stretched_buffer_[0], &stretched_buffer_[0] + stretched_length_, 0.0f);
    std::fill(&s_stretch_norm[0], &s_stretch_norm[0] + stretched_length_, 0.0f);

    is_stretching_ = true;
    use_stretched_buffer_ = false;
    s_synth_count = 0;
    s_stretch_state = StretchState::GATHER_FRAME;
}

void MicroLooperModule::ProcessStereo(float inL, float inR)
{
    m_audioLeft = inL;

    if (is_playing_ && mod_ > 0) {
        float speed = 4.0f * (GetParameterAsFloat(SPEED) - 0.5f);

        playing_head_.SetSpeed(speed);
        playing_head_.UpdatePosition(mod_);

        if (is_recording_) {
            recording_head_.UpdatePosition(mod_);
            size_t wraparound_count = recording_head_.GetWrapAroundCount();
            int mode = GetParameterAsBinnedValue(LOOP_MODE);
            if ((mode == SAMPLER) && (wraparound_count > prev_wraparound_count_)) {
                armed_stop_ = true;
                is_recording_ = false;
            }
            prev_wraparound_count_ = wraparound_count;
        }

        float playing_head_position_f = playing_head_.GetHeadPosition();
        size_t playing_head_position = static_cast<size_t>(playing_head_position_f);

        if (use_stretched_buffer_) {
            stretch_playing_head_.SetSpeed(speed);
            stretch_playing_head_.UpdatePosition(stretched_ready_length_);
            float stretch_playing_head_position_f = stretch_playing_head_.GetHeadPosition();
            size_t stretch_playing_head_position = static_cast<size_t>(stretch_playing_head_position_f);

            m_audioLeft += buffer_[playing_head_position] * GetParameterAsFloat(LOOP_MIX);
            m_audioLeft += stretched_buffer_[stretch_playing_head_position];
        } else {
            m_audioLeft += buffer_[playing_head_position] * GetParameterAsFloat(LOOP_MIX);
        }
    }

    m_audioRight = m_audioLeft;

    if (is_recording_) {
        WriteBuffer(inL);
    }
}

// ============================================================
// INCREMENTAL PROCESSING - Call from main loop
// ============================================================
bool MicroLooperModule::Poll() {
    // Looper
    if (armed_recording_) {
        if (midi_sync_) {
            if(clock_beat_) {
                ResetBuffer();
                armed_recording_ = false;
                clock_beat_ = false;
                is_recording_ = true;
                is_playing_ = true;
                prev_wraparound_count_ = 0;
            }
        } else {
            ResetBuffer();
            armed_recording_ = false;
            is_recording_ = true;
            is_playing_ = true;
            prev_wraparound_count_ = 0;
        }
    }

    bool immediate_stop = false;
    
    if (armed_stop_) {
        int mode = GetParameterAsBinnedValue(LOOP_MODE);
        if (midi_sync_ && (mode != SAMPLER)) {
            if(clock_beat_) {
                clock_beat_ = false;
                immediate_stop = true;
            }
        } else {
            immediate_stop = true;
        }
    }

    if (immediate_stop) {
        if (first_layer_) {
            first_layer_ = false;
            mod_ = loop_length_;
            loop_length_ = 0;
        }
        armed_stop_ = false;
        is_recording_ = false;
        is_playing_ = true;
        StartStretching();
    }

    if (is_stretching_) {
        switch(s_stretch_state) {
            case StretchState::IDLE:
                break;

            case StretchState::GATHER_FRAME:
                GatherFrameFromBuffer(s_snapshot_buffer, buffer_, mod_, stretch_read_pos_);
                s_stretch_state = StretchState::APPLY_ANALYSIS_WINDOW;
                break;

            case StretchState::APPLY_ANALYSIS_WINDOW:
                for(size_t k = 0; k < N; ++k) {
                    s_frame_time[k] = s_snapshot_buffer[k] * s_win[k];
                }
                s_stretch_state = StretchState::DO_FFT;
                break;

            case StretchState::DO_FFT:
                s_fft.Direct(s_frame_time, s_frame_freq);
                s_stretch_state = StretchState::EXTRACT_MAGNITUDES;
                break;

            case StretchState::EXTRACT_MAGNITUDES:
                s_magnitudes[0] = fabsf(s_frame_freq[0]);
                s_magnitudes[N/2 - 1] = fabsf(s_frame_freq[1]);
                for(size_t k = 1; k < N/2 - 1; ++k) {
                    float re = s_frame_freq[2*k];
                    float im = s_frame_freq[2*k + 1];
                    s_magnitudes[k] = sqrtf(re*re + im*im);
                }
                s_stretch_state = StretchState::RANDOMIZE_PHASES;
                break;

            case StretchState::RANDOMIZE_PHASES:
                s_frame_freq[0] = s_magnitudes[0];
                s_frame_freq[1] = s_magnitudes[N/2 - 1];

                for(size_t k = 1; k < N/2; ++k) {
                    float mag = s_magnitudes[k];

                    float u, v, r2;
                    do {
                        u = s_rng.randSigned();
                        v = s_rng.randSigned();
                        r2 = u*u + v*v;
                    } while(r2 > 1.0f || r2 < 1e-12f);

                    float inv = mag / sqrtf(r2);
                    s_frame_freq[2*k] = u * inv;
                    s_frame_freq[2*k + 1] = v * inv;
                }
                s_stretch_state = StretchState::DO_IFFT;
                break;

            case StretchState::DO_IFFT:
                s_fft.Inverse(s_frame_freq, s_frame_time);
                {
                    const float scale = 1.0f / (float)N;
                    for(size_t k = 0; k < N; ++k) {
                        s_frame_time[k] *= scale;
                    }
                }
                s_stretch_state = StretchState::APPLY_SYNTHESIS_WINDOW;
                break;

            case StretchState::APPLY_SYNTHESIS_WINDOW:
                for(size_t k = 0; k < N; ++k) {
                    s_result_buffer[k] = s_frame_time[k] * s_win[k];
                }
                s_stretch_state = StretchState::ADD_TO_OUTPUT;
                break;

            case StretchState::ADD_TO_OUTPUT:
                if (stretched_length_ > 0) {
                    OLA_AddFrame(stretched_buffer_, s_stretch_norm, stretched_length_, stretch_write_pos_,
                                 s_result_buffer, s_win2);
                    stretch_write_pos_ = (stretch_write_pos_ + H_OUT) % stretched_length_;
                }

                s_synth_count++;
                s_stretch_state = StretchState::CHECK_MORE_SYNTH;
                break;

            case StretchState::CHECK_MORE_SYNTH:
                if(s_synth_count < STRETCH) {
                    s_stretch_state = StretchState::RANDOMIZE_PHASES;
                } else {
                    s_synth_count = 0;
                    s_stretch_state = StretchState::ADVANCE_READ;
                }
                break;

            case StretchState::ADVANCE_READ:
                if (mod_ > 0) {
                    stretch_read_pos_ = (stretch_read_pos_ + H_IN) % mod_;
                } else {
                    stretch_read_pos_ = 0;
                }
                stretch_frames_done_++;

                use_stretched_buffer_ = (stretch_frames_done_ > 0);
                if (use_stretched_buffer_) {
                    stretched_ready_length_ = stretch_write_pos_;
                }

                if (stretch_frames_done_ >= stretch_total_frames_) {
                    s_stretch_state = StretchState::DONE;
                    stretched_ready_length_ = stretched_length_;
                } else {
                    s_stretch_state = StretchState::GATHER_FRAME;
                }
                break;

            case StretchState::DONE:
                if (stretched_length_ > 0) {
                    constexpr float eps = 1e-12f;
                    for (size_t i = 0; i < stretched_length_; ++i) {
                        float norm = s_stretch_norm[i];
                        stretched_buffer_[i] = (fabsf(norm) > eps) ? (stretched_buffer_[i] / norm) : 0.0f;
                    }
                }

                is_stretching_ = false;
                s_stretch_state = StretchState::IDLE;
                break;
        }
    }
    return true;
}

float MicroLooperModule::GetBrightnessForLED(int led_id) const
{
    if (led_id == 0)
    {
        // LED 0: recording indicator with fade in last 20% of loop
        if (is_recording_)
        {
            return 1.0f;
        }
        else
        {
            // Not recording: base off (pattern may override later)
            return 0.0f;
        }
    }
}
