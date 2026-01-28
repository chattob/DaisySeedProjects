#include "micro_looper_module.h"
#include "../Util/shy_fft.h"
#include <cmath>
#include <cstring>

using namespace bkshepherd;

float DSY_SDRAM_BSS MicroLooperModule::buffer_[kMicroLoopMaxSize];
float DSY_SDRAM_BSS MicroLooperModule::stretched_buffer_a_[kMicroLoopMaxStretchedSize];
float DSY_SDRAM_BSS MicroLooperModule::stretched_buffer_b_[kMicroLoopMaxStretchedSize];

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

constexpr float eps = 1e-12f;

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

// Normalization buffers for offline OLA (double-buffered)
static float DSY_SDRAM_BSS s_stretch_norm_a[kMicroLoopMaxStretchedSize];
static float DSY_SDRAM_BSS s_stretch_norm_b[kMicroLoopMaxStretchedSize];

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

static const int s_paramCount = 6;
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
        defaultValue : {.float_value = 1.0f},
        knobMapping : 0,
        midiCCMapping : -1
    },
    {
        name : "Input mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : 1,
        midiCCMapping : -1
    },
    {
        name : "Freeze mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : 1,
        midiCCMapping : -1
    },
    {
        name : "Loop mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.0f},
        knobMapping : 2,
        midiCCMapping : -1
    },
    {
        name : "Sensitivity",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 3,
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

    // Initialize envelope follower coefficients
    auto ms_to_coeff = [&](float ms) {
        float t = ms * 0.001f;
        return 1.0f - expf(-1.0f / (sample_rate * t));
    };
    a_att_ = ms_to_coeff(attack_ms_);
    a_rel_ = ms_to_coeff(release_ms_);

    // Pre-compute sample counts for auto-start timing
    start_hold_samps_ = static_cast<uint32_t>(sample_rate * start_hold_ms_ * 0.001f);
    rearm_samps_ = static_cast<uint32_t>(sample_rate * rearm_ms_ * 0.001f);

    // Initialize auto-start state
    env_ = 0.0f;
    above_count_ = 0;
    below_count_ = 0;
    auto_armed_ = true;
}

void MicroLooperModule::BypassFootswitchPressed() {
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
    streaming_stretch_  = false;
    stretch_read_pos_   = 0;
    stretch_write_pos_  = 0;
    stretch_total_frames_ = 0;
    stretch_frames_done_ = 0;
    stretch_output_frames_done_ = 0;
    stretch_clear_pending_ = false;
    stretch_clear_pos_ = 0;

    // Keep stretched buffer playback state (use_stretched_buffer_, active_stretch_buffer_)
    // so playback continues during re-recording.
    // Only reset the write buffer state - it will be set up by StartStretching()
    write_stretch_buffer_ = false;

    playing_head_.Reset();
    recording_head_.Reset();
    prev_wraparound_count_ = 0;

    s_stretch_state = StretchState::IDLE;
    s_synth_count = 0;

    // Reset auto-start counters (keep auto_armed_ state)
    above_count_ = 0;
    below_count_ = 0;
}

void MicroLooperModule::UpdateEnv(float x_abs)
{
    float a = (x_abs > env_) ? a_att_ : a_rel_;
    env_ += a * (x_abs - env_);
}

void MicroLooperModule::AutoStartLogic()
{
    if (!auto_start_enabled_) {
        return;
    }

    // Scale thresholds by sensitivity parameter (0-1)
    // sensitivity=0 → high threshold (hard to trigger)
    // sensitivity=1 → low threshold (easy to trigger)
    float sensitivity = GetParameterAsFloat(SENSITIVITY);

    if (sensitivity < eps) {
        return;
    }

    float thr_on = threshold_on_ + (threshold_on_min_ - threshold_on_) * sensitivity;
    float thr_off = thr_on * 0.6f;  // maintain hysteresis ratio

    if (!auto_armed_ || is_recording_) {
        // Re-arm when quiet for long enough
        if (env_ < thr_off) {
            if (++below_count_ >= rearm_samps_) {
                auto_armed_ = true;
                below_count_ = 0;
            }
        } else {
            below_count_ = 0;
        }
        return;
    }

    // Not recording, armed: wait for loudness
    if (env_ >= thr_on) {
        if (++above_count_ >= start_hold_samps_) {
            armed_recording_ = true;   // reuse existing start path in Poll()
            auto_armed_ = false;
            above_count_ = 0;
        }
    } else {
        above_count_ = 0;
    }
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
    // Always use streaming mode now
    streaming_stretch_ = true;
    stretch_read_pos_ = 0;
    stretch_write_pos_ = 0;
    stretch_frames_done_ = 0;
    stretch_output_frames_done_ = 0;
    stretch_total_frames_ = 0;  // Will be calculated when recording stops

    // Double-buffering: write to the inactive buffer
    // If we're currently using stretched buffer, keep it playing
    write_stretch_buffer_ = !active_stretch_buffer_;

    // Initialize the write buffer's state
    if (write_stretch_buffer_) {
        stretched_length_b_ = kMicroLoopMaxStretchedSize;
        stretched_ready_length_b_ = 0;
        stretched_buffer_normalized_b_ = false;
    } else {
        stretched_length_a_ = kMicroLoopMaxStretchedSize;
        stretched_ready_length_a_ = 0;
        stretched_buffer_normalized_a_ = false;
    }

    is_stretching_ = true;
    // Keep use_stretched_buffer_ = true if it was already true (seamless transition)
    s_synth_count = 0;
    s_stretch_state = StretchState::IDLE;
    stretch_clear_pending_ = true;
    stretch_clear_pos_ = 0;

    // Only reset playing head if we're not already using stretched buffer
    if (!use_stretched_buffer_) {
        stretch_playing_head_.Reset();
    }
}

float MicroLooperModule::ReadStretchedSample(size_t idx, bool normalized) {
    // During streaming, avoid dividing by partial norm (can blow up at window edges).
    (void)normalized;
    // Read from the active buffer
    if (active_stretch_buffer_) {
        return MicroLooperModule::stretched_buffer_b_[idx];
    } else {
        return MicroLooperModule::stretched_buffer_a_[idx];
    }
}

void MicroLooperModule::ProcessStereo(float inL, float inR)
{
    m_audioLeft = GetParameterAsFloat(IN_MIX) * inL;

    // Envelope follower + auto-start logic (runs every sample)
    float x = 0.5f * (fabsf(inL) + fabsf(inR));
    UpdateEnv(x);
    AutoStartLogic();

    // Write to buffer BEFORE updating recording head position
    if (is_recording_) {
        WriteBuffer(inL);
        if (!is_stretching_ && loop_length_ >= N) {
            StartStretching();
        }
    }

    if (is_playing_ && mod_ > 0) {
        float speed = GetParameterAsFloat(SPEED);
        playing_head_.SetSpeed(speed);

        // Read position BEFORE updating
        float playing_head_position_f = playing_head_.GetHeadPosition();
        size_t playing_head_position = static_cast<size_t>(playing_head_position_f);

        if (use_stretched_buffer_) {
            // Get length from active buffer
            size_t stretch_len;
            bool normalized;
            if (active_stretch_buffer_) {
                stretch_len = stretched_ready_length_b_;
                normalized = stretched_buffer_normalized_b_;
            } else {
                stretch_len = stretched_ready_length_a_;
                normalized = stretched_buffer_normalized_a_;
            }

            if (stretch_len > 0) {
                stretch_playing_head_.SetSpeed(speed);

                // Read position BEFORE updating
                float stretch_playing_head_position_f = stretch_playing_head_.GetHeadPosition();
                size_t stretch_playing_head_position = static_cast<size_t>(stretch_playing_head_position_f);
                if (stretch_playing_head_position >= stretch_len) {
                    stretch_playing_head_position %= stretch_len;
                }

                m_audioLeft += ReadStretchedSample(stretch_playing_head_position,
                                                   normalized) * GetParameterAsFloat(FREEZE_MIX);

                stretch_playing_head_.UpdatePosition(stretch_len);
            }
        }
        m_audioLeft += buffer_[playing_head_position] * GetParameterAsFloat(LOOP_MIX);

        // Update positions AFTER reading
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
            /*if ((loop_length_ >= N) && !is_stretching_) {
                StartStretching();
            }*/
        }
    }

    m_audioRight = m_audioLeft;
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
    }

    if (is_stretching_) {
        // Get pointers to write buffer and its norm buffer
        float* write_buffer = write_stretch_buffer_ ? stretched_buffer_b_ : stretched_buffer_a_;
        float* write_norm = write_stretch_buffer_ ? s_stretch_norm_b : s_stretch_norm_a;
        size_t write_length = write_stretch_buffer_ ? stretched_length_b_ : stretched_length_a_;

        if (stretch_clear_pending_) {
            size_t remaining = write_length - stretch_clear_pos_;
            size_t count = (remaining < kStretchClearChunk) ? remaining : kStretchClearChunk;
            if (count > 0) {
                std::fill(write_buffer + stretch_clear_pos_,
                          write_buffer + stretch_clear_pos_ + count, 0.0f);
                std::fill(write_norm + stretch_clear_pos_,
                          write_norm + stretch_clear_pos_ + count, 0.0f);
                stretch_clear_pos_ += count;
            }
            if (stretch_clear_pos_ >= write_length) {
                stretch_clear_pending_ = false;
                s_stretch_state = StretchState::GATHER_FRAME;
            }
            return true;
        }

        switch(s_stretch_state) {
            case StretchState::IDLE:
                break;

            case StretchState::GATHER_FRAME:
                {
                    // Use loop_length_ if still recording, otherwise mod_
                    size_t buffer_len = is_recording_ ? loop_length_ : mod_;
                    GatherFrameFromBuffer(s_snapshot_buffer, buffer_, buffer_len, stretch_read_pos_);
                    s_stretch_state = StretchState::APPLY_ANALYSIS_WINDOW;
                }
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
                if (write_length > 0 && (stretch_write_pos_ + N) <= write_length) {
                    OLA_AddFrame(write_buffer, write_norm, write_length, stretch_write_pos_,
                                 s_result_buffer, s_win2);
                    if (stretch_write_pos_ + H_OUT <= write_length) {
                        stretch_write_pos_ += H_OUT;
                    }
                    // Publish ready length every hop for earliest possible playback.
                    if (write_stretch_buffer_) {
                        stretched_ready_length_b_ = stretch_write_pos_;
                    } else {
                        stretched_ready_length_a_ = stretch_write_pos_;
                    }
                    // Switch to new buffer as soon as first data is ready
                    if (stretch_write_pos_ > 0) {
                        active_stretch_buffer_ = write_stretch_buffer_;
                        use_stretched_buffer_ = true;
                    }
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

            case StretchState::ADVANCE_READ:{
                // Use loop_length_ if still recording, otherwise mod_
                size_t available_samples = is_recording_ ? loop_length_ : mod_;

                // Calculate next read position
                size_t next_read_pos = stretch_read_pos_ + H_IN;

                // Check if we have enough samples for a full frame
                bool next_frame_available = (next_read_pos + N <= available_samples);
                bool recording_done = !is_recording_;

                if (next_frame_available) {
                    // Advance and continue processing
                    stretch_read_pos_ = next_read_pos;
                    stretch_frames_done_++;

                    s_stretch_state = StretchState::GATHER_FRAME;
                } else if (recording_done) {
                    // Recording stopped, finalize with actual length
                    stretch_total_frames_ = stretch_frames_done_;
                    size_t total_synth_frames = stretch_total_frames_ * STRETCH;
                    size_t final_length = total_synth_frames * H_OUT;
                    if (final_length > kMicroLoopMaxStretchedSize) {
                        final_length = kMicroLoopMaxStretchedSize;
                    }

                    // Update the write buffer's length
                    if (write_stretch_buffer_) {
                        stretched_length_b_ = final_length;
                        stretched_ready_length_b_ = final_length;
                    } else {
                        stretched_length_a_ = final_length;
                        stretched_ready_length_a_ = final_length;
                    }

                    if (final_length > 0) {
                        // Fold the OLA tail back to the start for a circular loop.
                        size_t fold_len = N - H_OUT;
                        if (fold_len > final_length) {
                            fold_len = final_length;
                        }
                        if (final_length + fold_len > kMicroLoopMaxStretchedSize) {
                            fold_len = kMicroLoopMaxStretchedSize - final_length;
                        }
                        for (size_t i = 0; i < fold_len; ++i) {
                            size_t tail_idx = final_length + i;
                            write_buffer[i] += write_buffer[tail_idx];
                            write_norm[i] += write_norm[tail_idx];
                            write_buffer[tail_idx] = 0.0f;
                            write_norm[tail_idx] = 0.0f;
                        }
                    }
                    s_stretch_state = StretchState::DONE;
                }
                // else: wait (stay in ADVANCE_READ until more data arrives)
                break;
            }

            case StretchState::DONE:{
                // Normalize the write buffer (which is now the active buffer)
                size_t final_length = write_stretch_buffer_ ? stretched_length_b_ : stretched_length_a_;
                if (final_length > 0) {
                    for (size_t i = 0; i < final_length; ++i) {
                        float norm = write_norm[i];
                        write_buffer[i] = (fabsf(norm) > eps) ? (write_buffer[i] / norm) : 0.0f;
                    }
                }

                // Mark write buffer as normalized
                if (write_stretch_buffer_) {
                    stretched_buffer_normalized_b_ = true;
                } else {
                    stretched_buffer_normalized_a_ = true;
                }

                is_stretching_ = false;
                streaming_stretch_ = false;
                s_stretch_state = StretchState::IDLE;
                break;
            }
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
