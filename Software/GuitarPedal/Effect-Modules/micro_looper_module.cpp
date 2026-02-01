#include "micro_looper_module.h"
#include "../Util/shy_fft.h"
#include <cmath>
#include <cstring>
#include "../Util/audio_utilities.h"

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
constexpr float kStretchFullOverlapRatio = 0.98f;
constexpr size_t kStretchDeclickSamples = 32;
constexpr size_t kStretchFadeInSamples = 24000;
constexpr size_t kStretchMinPlayLength = H_OUT * 2;

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
                                 size_t start, const float* x, const float* w2, bool reverse = false) {
    for(size_t i = 0; i < N; ++i) {
        size_t p = (start + i) % out_size;
        size_t src_i = reverse ? (N - 1 - i) : i;
        out[p] += x[src_i];
        norm[p] += w2[src_i];
    }
}

static inline void ComputeFullOverlapRange(const float* norm, size_t length,
                                           size_t& start, size_t& play_length) {
    start = 0;
    play_length = length;
    if (length < 2) {
        return;
    }

    float max_norm = 0.0f;
    float sum_norm = 0.0f;
    size_t count = 0;
    for (size_t i = 0; i < length; ++i) {
        float v = norm[i];
        if (v > max_norm) {
            max_norm = v;
        }
        if (v > eps) {
            sum_norm += v;
            ++count;
        }
    }
    if (max_norm <= eps) {
        return;
    }

    float ref_norm = max_norm;
    if (count > 0) {
        float avg_norm = sum_norm / static_cast<float>(count);
        if (max_norm > avg_norm * 1.05f) {
            ref_norm = avg_norm;
        }
    }

    float threshold = ref_norm * kStretchFullOverlapRatio;
    size_t first = 0;
    while (first < length && norm[first] < threshold) {
        ++first;
    }
    if (first == length) {
        return;
    }
    size_t last = length - 1;
    while (last > first && norm[last] < threshold) {
        --last;
    }

    size_t len = last - first + 1;
    if (len < 2) {
        return;
    }

    start = first;
    play_length = len;
}

static inline void UpdateStreamingPlayRange(const float* norm, size_t length,
                                            size_t& play_start, size_t& play_length,
                                            bool& locked) {
    size_t range_start = 0;
    size_t range_length = 0;
    ComputeFullOverlapRange(norm, length, range_start, range_length);
    if (range_length < kStretchMinPlayLength) {
        return;
    }

    if (!locked) {
        play_start = range_start;
        play_length = range_length;
        locked = true;
        return;
    }

    size_t play_end = play_start + play_length;
    size_t range_end = range_start + range_length;
    if (range_end > play_end && range_start <= play_end) {
        play_length = range_end - play_start;
    }
}

// ============================================================
// PARAMETER METADATA
// ============================================================
static const char *s_LoopModes[2] = {"Overdub", "Sampler"};

static const int s_paramCount = 7;
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
        name : "Slice",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : 0,
        midiCCMapping : -1
    },
    {
        name : "Fading",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.0f},
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
        name : "Balance",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 1,
        midiCCMapping : -1
    },
    {
        name : "Attack",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
        knobMapping : 3,
        midiCCMapping : -1
    },
    {
        name : "Sensitivity",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 1.0f},
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
    for (size_t i = 0; i < kMicroLoopMaxSize; ++i) {
        buffer_[i] = 0.0f;
    }

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
    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == OVERDUB) {
        if (!is_stretching_ && mod_ >= N) {
            StartStretching();
        }
        freeze_playing_ = true;
    } else {
        freeze_playing_ = !freeze_playing_;
    }
}

void MicroLooperModule::AlternateFootswitchPressed() {
    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == OVERDUB) {
         if (!is_recording_) {
            clock_beat_ = false;
            armed_recording_ = true;
        } else {
            clock_beat_ = false;
            armed_stop_ = true;
        }
        loop_playing_ = true;
    } else {
        loop_playing_ = !loop_playing_;
    }
}

void MicroLooperModule::FootswitchPressed(size_t footswitch_id) {
    switch (footswitch_id) {
        case 2:
            speed_error_ = true;
            break;
        case 3:
            break;
        case 4:
            SetParameterAsBinnedValue(LOOP_MODE, OVERDUB);
            break;
    }
}

void MicroLooperModule::FootswitchReleased(size_t footswitch_id) {
    switch (footswitch_id) {
        case 2:
            speed_error_ = false;
            break;
        case 3:
            break;
        case 4:
            SetParameterAsBinnedValue(LOOP_MODE, SAMPLER);
            break;
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
    stretch_clear_pending_ = false;
    stretch_clear_pos_ = 0;

    // Keep stretched buffer playback state (use_stretched_buffer_, active_stretch_buffer_)
    // so playback continues during re-recording.
    // Only reset the write buffer state - it will be set up by StartStretching()
    write_stretch_buffer_ = false;

    playing_head_.Reset();
    recording_head_.Reset();
    prev_wraparound_count_ = 0;
    samples_since_speed_change_ = 0;

    s_stretch_state = StretchState::IDLE;
    s_synth_count = 0;

    // Reset auto-start counters (keep auto_armed_ state)
    above_count_ = 0;
    below_count_ = 0;
    stretch_declick_count_ = 0;
    stretch_declick_prev_ = 0.0f;
    stretch_fade_in_count_ = 0;
}

void MicroLooperModule::ParameterChanged(int parameter_id) {
    if (parameter_id != LOOP_MODE) {
        return;
    }

    // Mode switch should be a clean slate to avoid stale state mixing.
    ResetBuffer();

    // Stop any pending actions.
    armed_recording_ = false;
    armed_stop_ = false;
    clock_beat_ = false;

    // Disable mode-specific playback toggles.
    loop_playing_ = false;
    freeze_playing_ = false;

    // Reset speed modulation state.
    speed_error_ = false;
    smoothed_speed_ = 1.0f;
    target_speed_ = 1.0f;
    samples_since_speed_change_ = 0;

    // Reset auto-start tracking.
    env_ = 0.0f;
    auto_armed_ = true;
    above_count_ = 0;
    below_count_ = 0;

    // Fully disable stretched playback and clear lengths.
    is_stretching_ = false;
    streaming_stretch_ = false;
    use_stretched_buffer_ = false;
    active_stretch_buffer_ = false;
    write_stretch_buffer_ = false;
    stretch_read_pos_ = 0;
    stretch_write_pos_ = 0;
    stretch_total_frames_ = 0;
    stretch_frames_done_ = 0;
    stretch_clear_pending_ = false;
    stretch_clear_pos_ = 0;
    stretched_length_a_ = 0;
    stretched_length_b_ = 0;
    stretched_ready_length_a_ = 0;
    stretched_ready_length_b_ = 0;
    stretched_play_start_a_ = 0;
    stretched_play_start_b_ = 0;
    stretched_play_length_a_ = 0;
    stretched_play_length_b_ = 0;
    stretched_play_locked_a_ = false;
    stretched_play_locked_b_ = false;
    stretched_buffer_normalized_a_ = false;
    stretched_buffer_normalized_b_ = false;
    stretch_declick_count_ = 0;
    stretch_declick_prev_ = 0.0f;
    stretch_fade_in_count_ = 0;
    stretch_speed_ = 1.0f;
    stretch_playing_head_.Reset();
    s_stretch_state = StretchState::IDLE;
    s_synth_count = 0;

    // Clear base loop buffer so fading doesn't pull old audio across modes.
    std::memset(buffer_, 0, sizeof(buffer_));
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

    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == OVERDUB) {
        float fading = GetParameterAsFloat(FADING);
        buffer_[recording_index] *= fading;
        buffer_[recording_index] += in;
    } else {
        buffer_[recording_index] = in;
    }

    // Cap length to the buffer size to avoid invalid lengths in OVERDUB mode.
    if (loop_length_ < kMicroLoopMaxSize) {
        loop_length_++;
    } else {
        loop_length_ = kMicroLoopMaxSize;
    }
};

void MicroLooperModule::StartStretching()
{
    // Always use streaming mode now
    streaming_stretch_ = true;
    stretch_read_pos_ = 0;
    stretch_write_pos_ = 0;
    stretch_frames_done_ = 0;
    stretch_total_frames_ = 0;  // Will be calculated when recording stops

    // Double-buffering: write to the inactive buffer
    // If we're currently using stretched buffer, keep it playing
    write_stretch_buffer_ = !active_stretch_buffer_;

    // Initialize the write buffer's state
    if (write_stretch_buffer_) {
        stretched_length_b_ = kMicroLoopMaxStretchedSize;
        stretched_ready_length_b_ = 0;
        stretched_play_start_b_ = 0;
        stretched_play_length_b_ = 0;
        stretched_play_locked_b_ = false;
        stretched_buffer_normalized_b_ = false;
    } else {
        stretched_length_a_ = kMicroLoopMaxStretchedSize;
        stretched_ready_length_a_ = 0;
        stretched_play_start_a_ = 0;
        stretched_play_length_a_ = 0;
        stretched_play_locked_a_ = false;
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
    stretch_fade_in_count_ = 0;
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

float MicroLooperModule::GetNextMarkovSpeed() {
    float current = target_speed_;
    // Random value in [0, 1)
    float r = (s_rng.nextU32() >> 8) * (1.0f / 16777216.0f);

    // Chaos factor: 0 = always stay at 1.0, 1 = 50% chance to leave 1.0
    // TODO: tie this to a knob
    float chaos = 1.0f;
    float stay_prob = 1.0f - chaos * 0.5f;  // Range: 1.0 (chaos=0) to 0.5 (chaos=1)

    if (current == 1.0f) {
        if (r < stay_prob) return 1.0f;
        // Distribute remaining probability
        // Weights: 2.0=25%, -2.0=25%, -1.0=25%, 0.5=12.5%, -0.5=12.5%
        float leave_r = (r - stay_prob) / (1.0f - stay_prob);  // Normalize to [0,1)
        if (leave_r < 0.25f) return 2.0f;
        else if (leave_r < 0.5f) return -2.0f;
        else if (leave_r < 0.75f) return -1.0f;
        else if (leave_r < 0.875f) return 0.5f;
        else return -0.5f;
    } else if (current == -1.0f) {
        // 1.0 has highest chance (50%), rest split equally
        if (r < 0.5f) return 1.0f;
        else if (r < 0.625f) return 0.5f;
        else if (r < 0.75f) return -0.5f;
        else if (r < 0.875f) return 2.0f;
        else return -2.0f;
    } else if (current == 0.5f || current == -0.5f) {
        // Bias toward 1.0
        if (r < 0.7f) return 1.0f;
        else return -1.0f;
    } else {
        // From 2.0 or -2.0: bias toward 1.0
        if (r < 0.7f) return 1.0f;
        else return -1.0f;
    }
}

void MicroLooperModule::ProcessStereo(float inL, float inR)
{
    m_audioLeft = GetParameterAsFloat(IN_MIX) * inL;
    float slice = GetParameterAsFloat(SLICE);
    auto gains = EnergyCrossfade(GetParameterAsFloat(BALANCE));
    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    float loop_mix = loop_playing_ ? gains.dry : 0.0f;
    float freeze_mix = freeze_playing_ ? gains.wet : 0.0f;

    if (mode == SAMPLER) {
        // Envelope follower + auto-start logic (runs every sample)
        float x = 0.5f * (fabsf(inL) + fabsf(inR));
        UpdateEnv(x);
        AutoStartLogic();
    }

    // Write to buffer BEFORE updating recording head position
    if (is_recording_) {
        WriteBuffer(inL);
        if (!is_stretching_ && loop_length_ >= N && mode == SAMPLER) {
            StartStretching();
        }
    }

    if (is_playing_ && mod_ > 0) {
        // Read position BEFORE updating
        float playing_head_position_f = playing_head_.GetHeadPosition();
        size_t playing_head_position = static_cast<size_t>(playing_head_position_f);

        if (use_stretched_buffer_) {
            // Get length from active buffer
            size_t stretch_len;
            size_t stretch_start = 0;
            bool normalized;
            if (active_stretch_buffer_) {
                stretch_len = (stretched_play_locked_b_ && stretched_play_length_b_ > 0)
                                  ? stretched_play_length_b_
                                  : stretched_ready_length_b_;
                stretch_start = (stretched_play_locked_b_ && stretched_play_length_b_ > 0)
                                    ? stretched_play_start_b_
                                    : 0;
                normalized = stretched_buffer_normalized_b_;
            } else {
                stretch_len = (stretched_play_locked_a_ && stretched_play_length_a_ > 0)
                                  ? stretched_play_length_a_
                                  : stretched_ready_length_a_;
                stretch_start = (stretched_play_locked_a_ && stretched_play_length_a_ > 0)
                                    ? stretched_play_start_a_
                                    : 0;
                normalized = stretched_buffer_normalized_a_;
            }

            if (stretch_len > 0) {
                // Read position BEFORE updating
                float stretch_playing_head_position_f = stretch_playing_head_.GetHeadPosition();
                size_t stretch_playing_head_position = static_cast<size_t>(stretch_playing_head_position_f);
                if (stretch_playing_head_position >= stretch_len) {
                    stretch_playing_head_position %= stretch_len;
                }

                size_t stretch_idx = stretch_start + stretch_playing_head_position;
                float stretch_sample = ReadStretchedSample(stretch_idx, normalized);
                if (stretch_declick_count_ > 0) {
                    float t = 1.0f - (stretch_declick_count_ / static_cast<float>(kStretchDeclickSamples));
                    float w = 0.5f - 0.5f * cosf(static_cast<float>(M_PI) * t);
                    stretch_sample = stretch_declick_prev_ * (1.0f - w) + stretch_sample * w;
                    stretch_declick_count_--;
                }
                if (stretch_fade_in_count_ > 0) {
                    size_t fade_in_samples = std::max(static_cast<size_t>(128), static_cast<size_t>(GetParameterAsFloat(ATTACK) * kStretchFadeInSamples));
                    float t = (fade_in_samples - stretch_fade_in_count_)
                              / static_cast<float>(fade_in_samples);
                    stretch_sample *= t;
                    stretch_fade_in_count_--;
                }
                m_audioLeft += stretch_sample * freeze_mix;

                bool bounced = stretch_playing_head_.UpdatePositionPingPong(stretch_len);
                if (bounced) {
                    stretch_declick_count_ = kStretchDeclickSamples;
                    stretch_declick_prev_ = stretch_sample;
                } else if (stretch_declick_count_ == 0) {
                    stretch_declick_prev_ = stretch_sample;
                }
            }
        }
        m_audioLeft += buffer_[playing_head_position] * loop_mix;

        // Update positions AFTER reading
        size_t wraparound_count = playing_head_.GetWrapAroundCount();
        playing_head_.UpdatePosition(mod_, slice);
        float speed;

        if (wraparound_count != playing_head_.GetWrapAroundCount()) {
            if (speed_error_ && samples_since_speed_change_ >= mod_ / 4) {
                target_speed_ = GetNextMarkovSpeed();
                samples_since_speed_change_ = 0;
            }
        }
        samples_since_speed_change_++;

        if (speed_error_) {
            // Low-pass filter for smooth speed transitions
            smoothed_speed_ += 0.002f * (target_speed_ - smoothed_speed_);
            speed = smoothed_speed_;
        } else {
            smoothed_speed_ = 1.0f;
            speed = 1.0f;
        }  
        playing_head_.SetSpeed(speed);
        stretch_playing_head_.SetSpeed(fabs(speed) * stretch_speed_);

        if (is_recording_) {
            recording_head_.UpdatePosition(mod_);
            wraparound_count = recording_head_.GetWrapAroundCount();
            int mode = GetParameterAsBinnedValue(LOOP_MODE);
            if ((mode == SAMPLER) && (wraparound_count > prev_wraparound_count_)) {
                armed_stop_ = true;
                is_recording_ = false;
            }
            prev_wraparound_count_ = wraparound_count;
        } else {
            size_t wraparound_count = playing_head_.GetWrapAroundCount();
            if (wraparound_count > prev_wraparound_count_) {

            }
            prev_wraparound_count_ = wraparound_count;
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
            mod_ = (loop_length_ > kMicroLoopMaxSize) ? kMicroLoopMaxSize : loop_length_;
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
                    bool reverse_previous_frame = (s_synth_count % 2 == 1);
                    OLA_AddFrame(write_buffer, write_norm, write_length, stretch_write_pos_,
                                 s_result_buffer, s_win2, reverse_previous_frame);
                    if (stretch_write_pos_ + H_OUT <= write_length) {
                        stretch_write_pos_ += H_OUT;
                    }
                }

                s_synth_count++;
                s_stretch_state = StretchState::CHECK_MORE_SYNTH;
                break;

            case StretchState::CHECK_MORE_SYNTH:
                if(s_synth_count < STRETCH) {
                    // First input frame  stretched - switch to new buffer
                    if (s_synth_count >= 6) {
                        if (write_stretch_buffer_) {
                            stretched_ready_length_b_ = stretch_write_pos_;
                            UpdateStreamingPlayRange(s_stretch_norm_b, stretched_ready_length_b_,
                                                     stretched_play_start_b_, stretched_play_length_b_,
                                                     stretched_play_locked_b_);
                        } else {
                            stretched_ready_length_a_ = stretch_write_pos_;
                            UpdateStreamingPlayRange(s_stretch_norm_a, stretched_ready_length_a_,
                                                     stretched_play_start_a_, stretched_play_length_a_,
                                                     stretched_play_locked_a_);
                        }
                        bool play_ready = false;
                        if (write_stretch_buffer_) {
                            play_ready = (stretched_play_locked_b_ &&
                                          stretched_play_length_b_ >= kStretchMinPlayLength);
                        } else {
                            play_ready = (stretched_play_locked_a_ &&
                                          stretched_play_length_a_ >= kStretchMinPlayLength);
                        }
                        if (play_ready && active_stretch_buffer_ != write_stretch_buffer_) {
                            active_stretch_buffer_ = write_stretch_buffer_;
                            use_stretched_buffer_ = true;
                            stretch_fade_in_count_ = std::max(static_cast<size_t>(128), static_cast<size_t>(GetParameterAsFloat(ATTACK) * kStretchFadeInSamples));

                        }
                    }
                    if (s_synth_count % 2 == 0) {
                        s_stretch_state = StretchState::RANDOMIZE_PHASES;
                    } else {
                        s_stretch_state = StretchState::ADD_TO_OUTPUT; // We will simply add the previous frame reverted.
                    }
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

                // Count the frame we just finished processing
                stretch_frames_done_++;

                if (next_frame_available) {
                    // Advance and continue processing
                    stretch_read_pos_ = next_read_pos;

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
                /*if (final_length > 0) {
                    for (size_t i = 0; i < final_length; ++i) {
                        float norm = write_norm[i];
                        write_buffer[i] = (fabsf(norm) > eps) ? (write_buffer[i] / norm) : 0.0f;
                    }
                }*/

                if (final_length > 0) {
                    size_t play_start = 0;
                    size_t play_length = final_length;
                    ComputeFullOverlapRange(write_norm, final_length, play_start, play_length);
                    if (write_stretch_buffer_) {
                        stretched_play_start_b_ = play_start;
                        stretched_play_length_b_ = play_length;
                        stretched_play_locked_b_ = (play_length > 0);
                    } else {
                        stretched_play_start_a_ = play_start;
                        stretched_play_length_a_ = play_length;
                        stretched_play_locked_a_ = (play_length > 0);
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
    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == SAMPLER) {
        if (led_id == 0) {
            return freeze_playing_ ? 1.0f : 0.0f;
        } else {
            return loop_playing_ ? 1.0f : 0.0f;
        }
    } else {
        if (led_id == 0) {
            return is_recording_ ? 1.0f : 0.0f;
        } else {
            return 0.0f;
        }
    }
}
