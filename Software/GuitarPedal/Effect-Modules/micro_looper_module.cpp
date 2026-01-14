#include "micro_looper_module.h"
#include "../Util/shy_fft.h"
#include <cmath>
#include <cstring>

using namespace bkshepherd;

float DSY_SDRAM_BSS MicroLooperModule::buffer_[kMicroLoopMaxSize];

// ============================================================
// FFT CONFIGURATION
// ============================================================
static constexpr size_t N = 16384;          // FFT size
static constexpr size_t H_IN = N / 2;      // Input hop (analysis)
static constexpr size_t STRETCH = 20;       // Stretch factor
static constexpr size_t H_OUT = N / 4;     // Output hop (synthesis)
static constexpr size_t OUT_RING = 4 * N;

// State machine for incremental processing
enum class ProcessState {
    IDLE,
    GATHER_INPUT,
    APPLY_ANALYSIS_WINDOW,
    DO_FFT,
    EXTRACT_MAGNITUDES,
    RANDOMIZE_PHASES,
    DO_IFFT,
    APPLY_SYNTHESIS_WINDOW,
    ADD_TO_OLA,
    CHECK_MORE_SYNTH
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

// Input ring buffer
static float DSY_SDRAM_BSS s_in_ring[N];
static size_t s_in_w = 0;
static size_t s_samples_seen = 0;
static size_t s_hop_in_counter = 0;

// Output OLA ring
static float DSY_SDRAM_BSS s_out_ring[OUT_RING];
static float DSY_SDRAM_BSS s_norm_ring[OUT_RING];
static size_t s_out_r = 0;
static size_t s_ola_w = 0;
static bool s_ola_inited = false;

// Window - in SDRAM
static float DSY_SDRAM_BSS s_win[N];
static float DSY_SDRAM_BSS s_win2[N];

// Processing state
static volatile ProcessState s_process_state = ProcessState::IDLE;
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

static inline void GatherFrameFromRing(float* dst, const float* ring, size_t ring_w) {
    size_t start = ring_w;
    for(size_t i = 0; i < N; ++i)
        dst[i] = ring[(start + i) % N];
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
static const int s_paramCount = 2;
static const ParameterMetaData s_metaData[s_paramCount] = {
    {
        name : "Cutoff",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 0,
        midiCCMapping : -1
    },
    {
        name : "HP Mode",
        valueType : ParameterValueType::Bool,
        valueBinCount : 0,
        defaultValue : {.uint_value = 0},
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
    std::memset(s_in_ring, 0, sizeof(s_in_ring));
    std::memset(s_out_ring, 0, sizeof(s_out_ring));
    std::memset(s_norm_ring, 0, sizeof(s_norm_ring));
    std::memset(s_magnitudes, 0, sizeof(s_magnitudes));

    // Reset state
    s_in_w = 0;
    s_samples_seen = 0;
    s_hop_in_counter = 0;
    s_out_r = 0;
    s_ola_w = 0;
    s_ola_inited = false;
    s_process_state = ProcessState::IDLE;
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

    playing_head_.Reset();
    recording_head_.Reset();

    std::fill(&buffer_[0], &buffer_[0] + kMicroLoopMaxSize, 0.0f);
}

void MicroLooperModule::WriteBuffer(float in)
{
    float recording_head_position_f = recording_head_.GetHeadPosition();
    size_t recording_index = static_cast<size_t>(recording_head_position_f);

    buffer_[recording_index] =  in;

    loop_length_++;
};

void MicroLooperModule::ProcessStereo(float inL, float inR)
{
    // Paulstretch
        // ---- Input ----
        s_in_ring[s_in_w] = inL;
        s_in_w = (s_in_w + 1) % N;
        s_samples_seen++;

        // ---- Output ----
        float acc = s_out_ring[s_out_r];
        float norm = s_norm_ring[s_out_r];
        s_out_ring[s_out_r] = 0.0f;
        s_norm_ring[s_out_r] = 0.0f;
        s_out_r = (s_out_r + 1) % OUT_RING;

        constexpr float eps = 1e-12f;
        float y = (fabsf(norm) > eps) ? (acc / norm) : 0.0f;
        m_audioLeft = y;
        m_audioRight = y;

        // ---- Trigger new analysis when input hop reached ----
        s_hop_in_counter++;
        if(s_hop_in_counter >= H_IN && s_samples_seen >= N) {
            if(s_process_state == ProcessState::IDLE) {
                s_hop_in_counter = 0;

                // Take snapshot
                GatherFrameFromRing(s_snapshot_buffer, s_in_ring, s_in_w);

                // Initialize OLA position
                if(!s_ola_inited) {
                    s_ola_w = (s_out_r + N) % OUT_RING;
                    s_ola_inited = true;
                }

                s_synth_count = 0;
                s_process_state = ProcessState::GATHER_INPUT;
            }
        }
    
        // Looper
    if (is_playing_) {
        float speed = 1.0;
        playing_head_.SetSpeed(speed);
        playing_head_.UpdatePosition(mod_);
        recording_head_.UpdatePosition(mod_);

        float playing_head_position_f = playing_head_.GetHeadPosition();
        size_t playing_head_position = static_cast<size_t>(playing_head_position_f);

        m_audioLeft += buffer_[playing_head_position];
    }
    
    m_audioLeft += inL;
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
            }
        } else {
            ResetBuffer();
            armed_recording_ = false;
            is_recording_ = true;
            is_playing_ = true;
        }
    }

    bool immediate_stop = false;
    
    if (armed_stop_) {
        if (midi_sync_) {
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
    }

    // Paulstretch
    switch(s_process_state) {

        case ProcessState::IDLE:
            break;

        case ProcessState::GATHER_INPUT:
            s_process_state = ProcessState::APPLY_ANALYSIS_WINDOW;
            break;

        case ProcessState::APPLY_ANALYSIS_WINDOW:
            for(size_t k = 0; k < N; ++k) {
                s_frame_time[k] = s_snapshot_buffer[k] * s_win[k];
            }
            s_process_state = ProcessState::DO_FFT;
            break;

        case ProcessState::DO_FFT:
            s_fft.Direct(s_frame_time, s_frame_freq);
            s_process_state = ProcessState::EXTRACT_MAGNITUDES;
            break;

        case ProcessState::EXTRACT_MAGNITUDES:
            s_magnitudes[0] = fabsf(s_frame_freq[0]);
            s_magnitudes[N/2 - 1] = fabsf(s_frame_freq[1]);
            for(size_t k = 1; k < N/2 - 1; ++k) {
                float re = s_frame_freq[2*k];
                float im = s_frame_freq[2*k + 1];
                s_magnitudes[k] = sqrtf(re*re + im*im);
            }
            s_process_state = ProcessState::RANDOMIZE_PHASES;
            break;

        case ProcessState::RANDOMIZE_PHASES:
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
            s_process_state = ProcessState::DO_IFFT;
            break;

        case ProcessState::DO_IFFT:
            s_fft.Inverse(s_frame_freq, s_frame_time);
            {
                const float scale = 1.0f / (float)N;
                for(size_t k = 0; k < N; ++k) {
                    s_frame_time[k] *= scale;
                }
            }
            s_process_state = ProcessState::APPLY_SYNTHESIS_WINDOW;
            break;

        case ProcessState::APPLY_SYNTHESIS_WINDOW:
            for(size_t k = 0; k < N; ++k) {
                s_result_buffer[k] = s_frame_time[k] * s_win[k];
            }
            s_process_state = ProcessState::ADD_TO_OLA;
            break;

        case ProcessState::ADD_TO_OLA:
            OLA_AddFrame(s_out_ring, s_norm_ring, OUT_RING, s_ola_w, s_result_buffer, s_win2);
            s_ola_w = (s_ola_w + H_OUT) % OUT_RING;

            s_synth_count++;
            s_process_state = ProcessState::CHECK_MORE_SYNTH;
            break;

        case ProcessState::CHECK_MORE_SYNTH:
            if(s_synth_count < STRETCH) {
                s_process_state = ProcessState::RANDOMIZE_PHASES;
            } else {
                s_process_state = ProcessState::IDLE;
            }
            break;
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