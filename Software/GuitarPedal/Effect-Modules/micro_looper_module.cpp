#include "micro_looper_module.h"
#include "../Util/shy_fft.h"
#include <cmath>
#include <cstring>
#include "../Util/audio_utilities.h"
#include "../Util/XorShift32.h"

using namespace bkshepherd;

float DSY_SDRAM_BSS MicroLooperModule::buffer_[kNumLoopLayers][kMicroLoopMaxSize];
float DSY_SDRAM_BSS MicroLooperModule::stretched_buffer_a_[kMicroLoopMaxStretchedSize];
float DSY_SDRAM_BSS MicroLooperModule::stretched_buffer_b_[kMicroLoopMaxStretchedSize];

#ifndef DSY_SRAM_BSS
// Project-local helper for buffers intentionally placed in AXI SRAM.
#define DSY_SRAM_BSS __attribute__((section(".sram_bss")))
#endif

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
constexpr size_t kStretchDeclickSamples = 128;
constexpr size_t kStretchFadeInSamples = 24000;
constexpr size_t kStretchMinPlayLength = H_OUT * 2;
constexpr size_t kStretchMinPingPongLength = H_OUT * 4;
constexpr float kStretchSwapFadeSeconds = 0.12f;
constexpr float kLoopHarmonySyncMs = 1000.0f;
constexpr uint32_t kRecordLedWrapBlinkMs = 380;
constexpr uint32_t kMidiSyncWaitBlinkMs = 50;
constexpr uint32_t kMidiPulseFlashMs = 12;
constexpr size_t kStretchWorkChunkSamples = 128;

static inline float Clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static inline float ResolveLoopSlice(float slice, int mode) {
    if (mode == MicroLooperModule::SAMPLER) {
        return kSamplerStretchSlice;
    }
    if (slice < kMicroLoopMinSlice) {
        return kMicroLoopMinSlice;
    }
    return slice;
}

static inline size_t ComputeSliceLength(float slice) {
    size_t slice_length = static_cast<size_t>(slice * kMicroLoopMaxSize);
    if (slice_length < N) {
        return N;
    }
    if (slice_length > kMicroLoopMaxSize) {
        return kMicroLoopMaxSize;
    }
    return slice_length;
}

static inline size_t ComputeStretchSourceLength(float slice, size_t loop_length) {
    size_t source_length = ComputeSliceLength(slice);
    if (loop_length > 0 && loop_length < source_length) {
        source_length = loop_length;
    }
    if (source_length < N) {
        source_length = N;
    }
    return source_length;
}

static inline size_t ComputeStretchScratchLength(size_t source_length) {
    if (source_length < N) {
        source_length = N;
    }
    size_t analysis_frames = ((source_length - N) / H_IN) + 1;
    size_t final_length = analysis_frames * STRETCH * H_OUT;
    size_t scratch_length = final_length + (N - H_OUT);
    if (scratch_length > kMicroLoopMaxStretchedSize) {
        scratch_length = kMicroLoopMaxStretchedSize;
    }
    return scratch_length;
}

static inline size_t ResolveStretchInputLength(bool is_recording,
                                               size_t recording_layer,
                                               size_t loop_length,
                                               size_t main_loop_length) {
    if (is_recording) {
        return (recording_layer == 0) ? loop_length : main_loop_length;
    }
    if (main_loop_length > 0) {
        return main_loop_length;
    }
    return loop_length;
}

static inline float ComputePitchRatio(float pitch_voice) {
    float clamped_voice = Clamp01(pitch_voice);
    float pitch_octaves = (clamped_voice * 2.0f) - 1.0f;
    return powf(2.0f, pitch_octaves);
}

static inline size_t ComputeStretchFadeInSamples(float attack, int mode) {
    if (mode == MicroLooperModule::SAMPLER && attack <= 0.0f) {
        return 0;
    }
    return 1024 + static_cast<size_t>(attack * kStretchFadeInSamples);
}

// ============================================================
// STATIC BUFFERS - memory placement tuned for stretch-noise mitigation
// ============================================================
static ShyFFT<float, N> s_fft;

// High-read working buffers in AXI SRAM (reduce SDRAM bus activity).
static float DSY_SRAM_BSS s_frame_time[N];
static float DSY_SRAM_BSS s_frame_freq[N];
// Lower-rate magnitude scratch stays in SDRAM.
static float DSY_SDRAM_BSS s_magnitudes[N/2];

// Large normalization buffers stay in SDRAM.
static float DSY_SDRAM_BSS s_stretch_norm_a[kMicroLoopMaxStretchedSize];
static float DSY_SDRAM_BSS s_stretch_norm_b[kMicroLoopMaxStretchedSize];

// Analysis/synthesis windows in AXI SRAM (read every frame/sample).
static float DSY_SRAM_BSS s_win[N];
static float DSY_SRAM_BSS s_win2[N];

// Processing state
static volatile StretchState s_stretch_state = StretchState::IDLE;
static size_t s_synth_count = 0;
static size_t s_stage_progress = 0;

// Frame snapshot/result scratch (large, sequential access) in SDRAM.
static float DSY_SDRAM_BSS s_snapshot_buffer[N];
static float DSY_SDRAM_BSS s_result_buffer[N];

static XorShift32 s_rng;

// ============================================================
// HELPER FUNCTIONS
// ============================================================
static void BuildHann(float* w, size_t n) {
    if (n == 0) {
        return;
    }
    if (n == 1) {
        w[0] = 1.0f;
        return;
    }
    float denom = static_cast<float>(n - 1);
    for(size_t i = 0; i < n; ++i) {
        w[i] = HannWeight(static_cast<float>(i) / denom);
    }
}

static inline void GatherFrameFromBuffer(float* dst, const float* buffer, size_t buffer_len,
                                         size_t start, size_t frame_offset, size_t frame_count) {
    if(buffer_len == 0) {
        std::fill(dst + frame_offset, dst + frame_offset + frame_count, 0.0f);
        return;
    }
    for(size_t i = 0; i < frame_count; ++i) {
        size_t src_idx = start + frame_offset + i;
        dst[frame_offset + i] = buffer[src_idx % buffer_len];
    }
}

static inline void OLA_AddFrame(float* out, float* norm, size_t out_size,
                                 size_t start, const float* x, const float* w2, bool reverse,
                                 size_t frame_offset, size_t frame_count) {
    for(size_t i = 0; i < frame_count; ++i) {
        size_t src_pos = frame_offset + i;
        size_t p = (start + src_pos) % out_size;
        size_t src_i = reverse ? (N - 1 - src_pos) : src_pos;
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

    // Once playback is active, keep the start fixed and only grow the tail.
    // This avoids read-index jumps while still allowing the loop to lengthen.
    size_t play_end = play_start + play_length;
    size_t range_end = range_start + range_length;
    if (range_end > play_end && range_end > play_start) {
        play_length = range_end - play_start;
    }
}

// ============================================================
// PARAMETER METADATA
// ============================================================
static const char *s_LoopModes[2] = {"Overdub", "Sampler"};

static constexpr int s_paramCount = MicroLooperModule::PARAM_COUNT;
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
    {
        name : "Pitch",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.5f},
        knobMapping : 2,
        midiCCMapping : -1
    },
    {
        name : "Pitch Mix",
        valueType : ParameterValueType::Float,
        valueBinCount : 0,
        defaultValue : {.float_value = 0.0f},
        knobMapping : -1,
        midiCCMapping : -1
    },
    {
        name : "Pitch Direction",
        valueType : ParameterValueType::Bool,
        valueBinCount : 0,
        defaultValue : {.uint_value = 1},
        knobMapping : -1,
        midiCCMapping : -1
    },
    {
        name : "Speed Error",
        valueType : ParameterValueType::Bool,
        valueBinCount : 0,
        defaultValue : {.uint_value = 0},
        knobMapping : -1,
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

void MicroLooperModule::OnMidiClockPulse() {
    midi_pulse_flash_until_ms_ = daisy::System::GetNow() + kMidiPulseFlashMs;
}

void MicroLooperModule::SetClockBeat() {
    clock_beat_ = true;
}

void MicroLooperModule::Init(float sample_rate)
{
    BaseEffectModule::Init(sample_rate);

    ResetStates();
    ClearTopLayers(0);
    has_committed_loop_ = false;
    recording_layer_ = 0;

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
    s_stage_progress = 0;

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
    if (kLoopHarmonySyncMs <= 0.0f) {
        harmony_sync_samples_ = 0;
    } else {
        harmony_sync_samples_ = static_cast<uint32_t>(sample_rate * kLoopHarmonySyncMs * 0.001f);
        if (harmony_sync_samples_ < 1) {
            harmony_sync_samples_ = 1;
        }
    }

    stretch_swap_fade_samples_ = static_cast<size_t>(sample_rate * kStretchSwapFadeSeconds);
    if (stretch_swap_fade_samples_ < 256) {
        stretch_swap_fade_samples_ = 256;
    }

    // Initialize auto-start state
    env_ = 0.0f;
    above_count_ = 0;
    below_count_ = 0;
    auto_armed_ = true;
}

void MicroLooperModule::AlternateFootswitchPressed() {
    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == OVERDUB) {
        size_t source_length = ResolveStretchInputLength(is_recording_, recording_layer_, loop_length_, main_loop_length_);
        // Short press should only start stretching when no stretched loop exists yet.
        if (!use_stretched_buffer_ && !is_stretching_ && (source_length >= N)) {
            StartStretching();
        }
        stretch_playing_ = true;
    } else {
        stretch_playing_ = !stretch_playing_;
    }
}

void MicroLooperModule::AlternateFootswitchDoubleTapped() {
    // Double tap on alternate footswitch stops stretched loop playback.
    stretch_playing_ = false;
}

void MicroLooperModule::AlternateFootswitchHeldFor1Second() {
    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == OVERDUB) {
        size_t source_length = ResolveStretchInputLength(is_recording_, recording_layer_, loop_length_, main_loop_length_);
        // Long press explicitly re-runs stretching for the current loop.
        if (!is_stretching_ && (source_length >= N)) {
            StartStretching();
        }
        stretch_playing_ = true;
        return;
    }

    // In SAMPLER mode, long press still clears stretched playback state.
    ResetStretchState(false);
    stretch_playing_ = false;
}

void MicroLooperModule::BypassFootswitchHeldFor1Second() {
    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == OVERDUB) {
        // Stop playback/recording and clear loop buffers for a clean restart.
        armed_recording_ = false;
        armed_stop_ = false;
        clock_beat_ = false;
        FinalizeRecording(false);

        const bool keep_stretch_playing = use_stretched_buffer_ && stretch_playing_;
        ResetLoopState();
        if (keep_stretch_playing) {
            stretch_playing_ = true;
        }

        env_ = 0.0f;
        auto_armed_ = true;

        ClearTopLayers(0);
        has_committed_loop_ = false;
        main_loop_length_ = 0;
        recording_layer_ = 0;
    }
}

void MicroLooperModule::BypassFootswitchDoubleTapped() {
    loop_playing_ = false;
    armed_recording_ = false;
    armed_stop_ = false;
    clock_beat_ = false;

    if (is_recording_) {
        if (is_stretching_) {
            size_t source_length = (recording_layer_ == 0) ? loop_length_ : main_loop_length_;
            if (source_length == 0) {
                source_length = kMicroLoopMaxSize;
            }
            stretch_source_wrap_length_ = ComputeStretchSourceLength(stretch_slice_, source_length);
        }
        FinalizeRecording(true);
        is_recording_ = false;
    }
}

void MicroLooperModule::BypassFootswitchPressed() {
    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == OVERDUB) {
        if (!is_recording_) {
            clock_beat_ = false;
            armed_stop_ = false;
            armed_recording_ = true;
            // While waiting for quantized start, only keep playback running if
            // there is already a committed loop to overdub.
            loop_playing_ = has_committed_loop_;
        } else {
            // Allow recording to start while stretching (records to layer 1),
            // but prevent stop requests while stretching on the overdub layer
            // to avoid squashing it into the base layer mid-stretch.
            if (is_stretching_ && recording_layer_ > 0) {
                return;
            }
            armed_recording_ = false;
            clock_beat_ = false;
            armed_stop_ = true;
        }
    } else {
        loop_playing_ = !loop_playing_;
    }
}

void MicroLooperModule::ResetLoopState(bool preserve_playheads) {
    loop_playing_       = false;
    is_recording_       = false;
    armed_stop_         = false;
    clock_beat_         = false;
    recording_layer_    = 0;
    loop_length_        = 0;
    record_led_blink_until_ms_ = 0;

    if (!preserve_playheads) {
        playing_head_.Reset();
        loop_harmony_head_.Reset();
        prev_wraparound_count_ = 0;
    } else {
        prev_wraparound_count_ = playing_head_.GetWrapAroundCount();
    }
    samples_since_speed_change_ = 0;
}

void MicroLooperModule::ResetStretchState(bool preserve_playback) {
    is_stretching_      = false;
    stretch_slice_      = 1.0f;
    stretch_source_wrap_length_ = 0;
    stretch_read_pos_   = 0;
    stretch_write_pos_  = 0;
    stretch_total_frames_ = 0;
    stretch_frames_done_ = 0;
    stretch_clear_pending_ = false;
    stretch_clear_pos_ = 0;
    stretch_declick_count_ = 0;
    stretch_declick_prev_ = 0.0f;
    stretch_harmony_declick_count_ = 0;
    stretch_harmony_declick_prev_ = 0.0f;
    stretch_fade_in_count_ = 0;
    stretch_swap_fade_count_ = 0;
    stretch_swap_prev_buffer_ = false;

    s_stretch_state = StretchState::IDLE;
    s_synth_count = 0;

    // Keep stretched buffer playback state (use_stretched_buffer_, active_stretch_buffer_)
    // so playback continues during re-recording.
    // Only reset the write buffer state - it will be set up by StartStretching()
    if (preserve_playback) {
        write_stretch_buffer_ = false;
        return;
    }

    stretch_playing_ = false;
    use_stretched_buffer_ = false;
    active_stretch_buffer_ = false;
    write_stretch_buffer_ = false;
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
    stretch_speed_ = 1.0f;
    stretch_playing_head_.Reset();
    stretch_harmony_head_.Reset();
}

void MicroLooperModule::ResetAutoStartCounters() {
    above_count_ = 0;
    below_count_ = 0;
}

void MicroLooperModule::ClearTopLayers(size_t clear_from) {
    if (clear_from >= kNumLoopLayers) {
        return;
    }
    for (size_t layer = clear_from; layer < kNumLoopLayers; ++layer) {
        std::fill(&buffer_[layer][0], &buffer_[layer][0] + kMicroLoopMaxSize, 0.0f);
    }
}

void MicroLooperModule::SquashLayers() {
    // With two layers: fold temporary overdub layer into base layer.
    for (size_t sample = 0; sample < kMicroLoopMaxSize; ++sample) {
        buffer_[0][sample] += buffer_[1][sample];
    }
    std::fill(&buffer_[1][0], &buffer_[1][0] + kMicroLoopMaxSize, 0.0f);
}

void MicroLooperModule::FinalizeRecording(bool commit_recording_layer) {
    if (recording_layer_ > 0) {
        if (commit_recording_layer) {
            SquashLayers();
            has_committed_loop_ = true;
        } else {
            ClearTopLayers(recording_layer_);
        }
        recording_layer_ = 0;
    } else if (loop_length_ > 0 && commit_recording_layer) {
        main_loop_length_ = std::min(loop_length_, static_cast<size_t>(kMicroLoopMaxSize));
        has_committed_loop_ = true;
    }
}

void MicroLooperModule::ResetStates(bool preserve_playheads) {
    ResetLoopState(preserve_playheads);
    ResetStretchState(true);

    // Reset auto-start counters (keep auto_armed_ state)
    ResetAutoStartCounters();
}

void MicroLooperModule::ParameterChanged(int parameter_id) {
    if (parameter_id == SPEED_ERROR) {
        speed_error_ = GetParameterAsBool(SPEED_ERROR);
        if (!speed_error_) {
            smoothed_speed_ = 1.0f;
            target_speed_ = 1.0f;
            samples_since_speed_change_ = 0;
        }
        return;
    }

    if (parameter_id != LOOP_MODE) {
        return;
    }

    // Mode switch should be a clean slate to avoid stale state mixing.
    ResetLoopState();
    ResetStretchState(false);
    ResetAutoStartCounters();

    // Stop any pending actions.
    armed_recording_ = false;

    // Disable mode-specific playback toggles.
    loop_playing_ = false;
    stretch_playing_ = false;

    // Reset speed modulation state.
    speed_error_ = GetParameterAsBool(SPEED_ERROR);
    smoothed_speed_ = 1.0f;
    target_speed_ = 1.0f;
    samples_since_speed_change_ = 0;

    // Reset auto-start tracking.
    env_ = 0.0f;
    auto_armed_ = true;

    // Clear base loop buffer so fading doesn't pull old audio across modes.
    ClearTopLayers(0);
    has_committed_loop_ = false;
    main_loop_length_ = 0;
    recording_layer_ = 0;
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
    float head_position_f = playing_head_.GetHeadPosition();
    size_t write_index = static_cast<size_t>(head_position_f);

    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == OVERDUB) {
        float fading = GetParameterAsFloat(FADING);
        buffer_[recording_layer_][write_index] *= fading;
        buffer_[recording_layer_][write_index] += in;
    } else {
        buffer_[0][write_index] = in;
    }

    // Main loop length is only defined by the base layer recording pass.
    if (recording_layer_ == 0) {
        if (loop_length_ < kMicroLoopMaxSize) {
            loop_length_++;
        } else {
            loop_length_ = kMicroLoopMaxSize;
        }
    }
};

void MicroLooperModule::StartStretching()
{
    const int mode = GetParameterAsBinnedValue(LOOP_MODE);

    // Latch slice so source sizing stays stable during stretching.
    stretch_slice_ = ResolveLoopSlice(GetParameterAsFloat(SLICE), mode);

    size_t source_length = ResolveStretchInputLength(is_recording_, recording_layer_, loop_length_, main_loop_length_);
    if (source_length == 0) {
        source_length = kMicroLoopMaxSize;
    }
    stretch_source_wrap_length_ = ComputeStretchSourceLength(stretch_slice_, source_length);

    // Stretching should lock in a fixed source length immediately.
    // If we were recording, force-stop and commit first.
    if (is_recording_) {
        armed_recording_ = false;
        FinalizeRecording(true);
        is_recording_ = false;
        if (mode != SAMPLER) {
            loop_playing_ = true;
        }
    }

    // Always use streaming mode now
    stretch_read_pos_ = 0;
    stretch_write_pos_ = 0;
    stretch_frames_done_ = 0;
    stretch_total_frames_ = 0;  // Will be calculated when recording stops

    // Double-buffering: write to the inactive buffer
    // If we're currently using stretched buffer, keep it playing
    write_stretch_buffer_ = !active_stretch_buffer_;

    // Initialize the write buffer's state.
    // Clear/write only the scratch span needed for this source length.
    size_t stretch_scratch_length = ComputeStretchScratchLength(stretch_source_wrap_length_);
    if (write_stretch_buffer_) {
        stretched_length_b_ = stretch_scratch_length;
        stretched_ready_length_b_ = 0;
        stretched_play_start_b_ = 0;
        stretched_play_length_b_ = 0;
        stretched_play_locked_b_ = false;
    } else {
        stretched_length_a_ = stretch_scratch_length;
        stretched_ready_length_a_ = 0;
        stretched_play_start_a_ = 0;
        stretched_play_length_a_ = 0;
        stretched_play_locked_a_ = false;
    }

    is_stretching_ = true;
    // Keep use_stretched_buffer_ = true if it was already true (seamless transition)
    s_synth_count = 0;
    s_stage_progress = 0;
    s_stretch_state = StretchState::IDLE;
    stretch_clear_pending_ = true;
    stretch_clear_pos_ = 0;

    // Only reset playing head if we're not already using stretched buffer
    if (!use_stretched_buffer_) {
        stretch_playing_head_.Reset();
        stretch_harmony_head_.Reset();
    }
    stretch_fade_in_count_ = 0;
}

float MicroLooperModule::ReadStretchedSample(size_t idx) {
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
    BaseEffectModule::ProcessStereo(inL, inR);

    if (m_isEnabled) {
        m_audioLeft = GetParameterAsFloat(IN_MIX) * inL;
        int mode = GetParameterAsBinnedValue(LOOP_MODE);
        float slice = ResolveLoopSlice(GetParameterAsFloat(SLICE), mode);
        auto gains = EnergyCrossfade(GetParameterAsFloat(BALANCE));
        float loop_mix = gains.dry;
        float freeze_mix = gains.wet;
        const bool recording_main_loop = is_recording_ && (recording_layer_ == 0);
        size_t playback_loop_length = static_cast<size_t>(kMicroLoopMaxSize);
        float playback_slice = slice;
        if (mode == OVERDUB) {
            playback_loop_length = (main_loop_length_ > 0)
                                       ? main_loop_length_
                                       : static_cast<size_t>(kMicroLoopMaxSize);
            if (recording_main_loop) {
                // Keep first pass recording open-ended until explicit stop.
                playback_loop_length = kMicroLoopMaxSize;
                playback_slice = 1.0f;
            }
        }

        if (mode == SAMPLER) {
            // Envelope follower + auto-start logic (runs every sample)
            float x = 0.5f * (fabsf(inL) + fabsf(inR));
            UpdateEnv(x);
            AutoStartLogic();
        }

        // Write to buffer BEFORE updating recording head position
        if (is_recording_) {
            WriteBuffer(inL);
            if (!is_stretching_ && loop_length_ >= kSamplerStretchSourceLength && mode == SAMPLER) {
                StartStretching();
            }
        }

        // Sampler capture still needs the write head to move when loop playback
        // is muted, otherwise each input sample overwrites the same slot and the
        // stretch source never fills.
        if (mode == SAMPLER && is_recording_ && !loop_playing_) {
            playing_head_.SetSpeed(1.0f);
            playing_head_.UpdatePosition(playback_loop_length, playback_slice);
            loop_harmony_head_.SyncTo(playing_head_, playback_loop_length, 0.0f);
            prev_wraparound_count_ = playing_head_.GetWrapAroundCount();
        }

        if (loop_playing_ || stretch_playing_) {
            float harmony_mix = Clamp01(GetParameterAsFloat(PITCH_MIX));
            auto harmony_gains = EnergyCrossfade(harmony_mix);
            bool harmony_forward = GetParameterAsBool(PITCH_DIRECTION);
            float pitch_ratio = ComputePitchRatio(GetParameterAsFloat(PITCH_VOICE));
            if (speed_error_) {
                // Low-pass filter for smooth speed transitions.
                smoothed_speed_ += 0.002f * (target_speed_ - smoothed_speed_);
                pitch_ratio *= smoothed_speed_;
            }

            if (stretch_playing_ && use_stretched_buffer_) {
                // Get length from active buffer
                size_t stretch_len;
                size_t stretch_start = 0;
                if (active_stretch_buffer_) {
                    stretch_len = (stretched_play_locked_b_ && stretched_play_length_b_ > 0)
                                    ? stretched_play_length_b_
                                    : stretched_ready_length_b_;
                    stretch_start = (stretched_play_locked_b_ && stretched_play_length_b_ > 0)
                                        ? stretched_play_start_b_
                                        : 0;
                } else {
                    stretch_len = (stretched_play_locked_a_ && stretched_play_length_a_ > 0)
                                    ? stretched_play_length_a_
                                    : stretched_ready_length_a_;
                    stretch_start = (stretched_play_locked_a_ && stretched_play_length_a_ > 0)
                                        ? stretched_play_start_a_
                                        : 0;
                }

                if (stretch_len > 0) {
                    // Read position BEFORE updating
                    float stretch_playing_head_position_f = stretch_playing_head_.GetHeadPosition();
                    size_t stretch_playing_head_position = static_cast<size_t>(stretch_playing_head_position_f);
                    if (stretch_playing_head_position >= stretch_len) {
                        stretch_playing_head_position %= stretch_len;
                    }

                    size_t stretch_idx = stretch_start + stretch_playing_head_position;
                    float stretch_sample = ReadStretchedSample(stretch_idx);
                    float stretch_harmony_head_position_f = stretch_harmony_head_.GetHeadPosition();
                    size_t stretch_harmony_head_position = static_cast<size_t>(stretch_harmony_head_position_f);
                    if (stretch_harmony_head_position >= stretch_len) {
                        stretch_harmony_head_position %= stretch_len;
                    }

                    size_t stretch_harmony_idx = stretch_start + stretch_harmony_head_position;
                    float stretch_harmony_sample = ReadStretchedSample(stretch_harmony_idx);
                    if (stretch_swap_fade_count_ > 0 && stretch_swap_fade_samples_ > 0) {
                        size_t prev_len = 0;
                        size_t prev_start = 0;
                        if (stretch_swap_prev_buffer_) {
                            prev_len = (stretched_play_locked_b_ && stretched_play_length_b_ > 0)
                                        ? stretched_play_length_b_
                                        : stretched_ready_length_b_;
                            prev_start = (stretched_play_locked_b_ && stretched_play_length_b_ > 0)
                                            ? stretched_play_start_b_
                                            : 0;
                        } else {
                            prev_len = (stretched_play_locked_a_ && stretched_play_length_a_ > 0)
                                        ? stretched_play_length_a_
                                        : stretched_ready_length_a_;
                            prev_start = (stretched_play_locked_a_ && stretched_play_length_a_ > 0)
                                            ? stretched_play_start_a_
                                            : 0;
                        }

                        if (prev_len > 0) {
                            size_t prev_pos = stretch_playing_head_position;
                            if (prev_pos >= prev_len) {
                                prev_pos %= prev_len;
                            }
                            size_t prev_idx = prev_start + prev_pos;
                            float prev_sample = stretch_swap_prev_buffer_
                                                    ? stretched_buffer_b_[prev_idx]
                                                    : stretched_buffer_a_[prev_idx];
                            size_t prev_harmony_pos = stretch_harmony_head_position;
                            if (prev_harmony_pos >= prev_len) {
                                prev_harmony_pos %= prev_len;
                            }
                            size_t prev_harmony_idx = prev_start + prev_harmony_pos;
                            float prev_harmony_sample = stretch_swap_prev_buffer_
                                                            ? stretched_buffer_b_[prev_harmony_idx]
                                                            : stretched_buffer_a_[prev_harmony_idx];
                            float t = (stretch_swap_fade_samples_ - stretch_swap_fade_count_)
                                    / static_cast<float>(stretch_swap_fade_samples_);
                            float w = 0.5f - 0.5f * cosf(static_cast<float>(M_PI) * t);
                            stretch_sample = prev_sample * (1.0f - w) + stretch_sample * w;
                            stretch_harmony_sample = prev_harmony_sample * (1.0f - w) + stretch_harmony_sample * w;
                            stretch_swap_fade_count_--;
                        } else {
                            stretch_swap_fade_count_ = 0;
                        }
                    }
                    if (stretch_declick_count_ > 0) {
                        float t = 1.0f - (stretch_declick_count_ / static_cast<float>(kStretchDeclickSamples));
                        float w = 0.5f - 0.5f * cosf(static_cast<float>(M_PI) * t);
                        stretch_sample = stretch_declick_prev_ * (1.0f - w) + stretch_sample * w;
                        stretch_declick_count_--;
                    }
                    if (stretch_harmony_declick_count_ > 0) {
                        float t = 1.0f - (stretch_harmony_declick_count_ / static_cast<float>(kStretchDeclickSamples));
                        float w = 0.5f - 0.5f * cosf(static_cast<float>(M_PI) * t);
                        stretch_harmony_sample = stretch_harmony_declick_prev_ * (1.0f - w) + stretch_harmony_sample * w;
                        stretch_harmony_declick_count_--;
                    }
                    if (stretch_fade_in_count_ > 0) {
                        size_t fade_in_samples = ComputeStretchFadeInSamples(GetParameterAsFloat(ATTACK), mode);
                        if (fade_in_samples == 0) {
                            stretch_fade_in_count_ = 0;
                        } else {
                            float t = (fade_in_samples - stretch_fade_in_count_)
                                    / static_cast<float>(fade_in_samples);
                            stretch_sample *= t;
                            stretch_harmony_sample *= t;
                            stretch_fade_in_count_--;
                        }
                    }
                    float stretch_mix_sample = (stretch_sample * harmony_gains.dry)
                                             + (stretch_harmony_sample * harmony_gains.wet);
                    m_audioLeft += stretch_mix_sample * freeze_mix;

                    bool bounced = false;
                    size_t stretch_wraparound_count = stretch_playing_head_.GetWrapAroundCount();
                    if (stretch_len >= kStretchMinPingPongLength) {
                        bounced = stretch_playing_head_.UpdatePositionPingPong(stretch_len);
                    } else {
                        stretch_playing_head_.UpdatePosition(stretch_len);
                    }
                    size_t updated_stretch_wraparound_count = stretch_playing_head_.GetWrapAroundCount();
                    if (bounced) {
                        stretch_declick_count_ = kStretchDeclickSamples;
                        stretch_declick_prev_ = stretch_sample;
                    } else if (stretch_declick_count_ == 0) {
                        stretch_declick_prev_ = stretch_sample;
                    }

                    bool harmony_bounced = false;
                    if (stretch_len >= kStretchMinPingPongLength) {
                        harmony_bounced = stretch_harmony_head_.UpdatePositionPingPong(stretch_len);
                    } else {
                        stretch_harmony_head_.UpdatePosition(stretch_len);
                    }
                    if (harmony_bounced) {
                        stretch_harmony_declick_count_ = kStretchDeclickSamples;
                        stretch_harmony_declick_prev_ = stretch_harmony_sample;
                    } else if (stretch_harmony_declick_count_ == 0) {
                        stretch_harmony_declick_prev_ = stretch_harmony_sample;
                    }

                    if (!loop_playing_) {
                        if (stretch_wraparound_count != updated_stretch_wraparound_count) {
                            if (speed_error_ && samples_since_speed_change_ >= stretch_len / 4) {
                                target_speed_ = GetNextMarkovSpeed();
                                samples_since_speed_change_ = 0;
                            }
                        }
                        samples_since_speed_change_++;
                    }
                }
            }
            if (loop_playing_) {
                // Read position BEFORE updating
                float playing_head_position_f = playing_head_.GetHeadPosition();
                size_t playing_head_position = static_cast<size_t>(playing_head_position_f);
                if (playing_head_position >= playback_loop_length) {
                    playing_head_position %= playback_loop_length;
                }
                float loop_harmony_head_position_f = loop_harmony_head_.GetHeadPosition();
                size_t loop_harmony_head_position = static_cast<size_t>(loop_harmony_head_position_f);
                if (loop_harmony_head_position >= playback_loop_length) {
                    loop_harmony_head_position %= playback_loop_length;
                }
                float loop_sample = buffer_[0][playing_head_position];
                float loop_harmony_sample = buffer_[0][loop_harmony_head_position];
                if (is_recording_ && recording_layer_ > 0) {
                    loop_sample += buffer_[recording_layer_][playing_head_position];
                    loop_harmony_sample += buffer_[recording_layer_][loop_harmony_head_position];
                }
                float loop_mix_sample = (loop_sample * harmony_gains.dry)
                                      + (loop_harmony_sample * harmony_gains.wet);
                m_audioLeft += loop_mix_sample * loop_mix;

                // Update positions AFTER reading
                size_t wraparound_count = playing_head_.GetWrapAroundCount();
                playing_head_.UpdatePosition(playback_loop_length, playback_slice);
                loop_harmony_head_.UpdatePosition(playback_loop_length, playback_slice);
                size_t updated_wraparound_count = playing_head_.GetWrapAroundCount();
                float speed;

                if (wraparound_count != updated_wraparound_count) {
                    record_led_blink_until_ms_ = daisy::System::GetNow() + kRecordLedWrapBlinkMs;
                    if (speed_error_ && samples_since_speed_change_ >= playback_loop_length / 4) {
                        target_speed_ = GetNextMarkovSpeed();
                        samples_since_speed_change_ = 0;
                    }
                }
                samples_since_speed_change_++;

                speed = 1.0f;
                playing_head_.SetSpeed(speed);
                stretch_playing_head_.SetSpeed(fabs(speed) * stretch_speed_);
                stretch_harmony_head_.SetSpeed(fabs(speed) * stretch_speed_ * pitch_ratio);
                float harmony_dir = harmony_forward ? 1.0f : -1.0f;
                loop_harmony_head_.SetSpeed(harmony_dir * fabs(speed) * pitch_ratio);

                // In SAMPLER mode, stop recording automatically at loop wrap.
                if (is_recording_ && mode == SAMPLER && (updated_wraparound_count > prev_wraparound_count_)) {
                    if (is_stretching_) {
                        size_t source_length = (recording_layer_ == 0) ? loop_length_ : main_loop_length_;
                        if (source_length == 0) {
                            source_length = kMicroLoopMaxSize;
                        }
                        stretch_source_wrap_length_ = ComputeStretchSourceLength(stretch_slice_, source_length);
                    }
                    FinalizeRecording(true);
                    is_recording_ = false;
                }
                prev_wraparound_count_ = updated_wraparound_count;
            } else if (stretch_playing_) {
                stretch_playing_head_.SetSpeed(stretch_speed_);
                stretch_harmony_head_.SetSpeed(stretch_speed_ * pitch_ratio);
            }
        }

        m_audioRight = m_audioLeft;
    }
}

// ============================================================
// INCREMENTAL PROCESSING - Call from main loop
// ============================================================
bool MicroLooperModule::Poll() {
    // Treat MIDI clock beat as an edge when not waiting for quantized start/stop.
    // This prevents stale beats from causing immediate (unquantized-looking) starts.
    if (!armed_recording_ && !armed_stop_) {
        clock_beat_ = false;
    }

    // Looper
    if (armed_recording_) {
        bool immediate_start = false;
        if (midi_clock_running_) {
            if (clock_beat_) {
                immediate_start = true;
                clock_beat_ = false;
            }
        } else {
            immediate_start = true;
        }

        if (immediate_start) {
            int mode = GetParameterAsBinnedValue(LOOP_MODE);
            const bool keep_loop_playing = (mode == SAMPLER) ? loop_playing_ : true;
            const bool keep_stretch_playing = stretch_playing_;
            bool overdub_existing_loop = (mode == OVERDUB) && has_committed_loop_;
            bool preserve_playheads = (mode == OVERDUB) && overdub_existing_loop;
            if (overdub_existing_loop) {
                // Keep stretch state alive when starting an overdub layer.
                ResetLoopState(preserve_playheads);
                ResetAutoStartCounters();
                recording_layer_ = 1;
                ClearTopLayers(recording_layer_);
            } else {
                ResetStates(preserve_playheads);
                recording_layer_ = 0;
                main_loop_length_ = 0;
                has_committed_loop_ = false;
                // Fresh base recording must always start from a clean buffer region.
                ClearTopLayers(0);
            }
            size_t sync_length = static_cast<size_t>(kMicroLoopMaxSize);
            if (mode == OVERDUB && main_loop_length_ > 0) {
                sync_length = main_loop_length_;
            }
            loop_harmony_head_.SyncTo(playing_head_, sync_length, static_cast<float>(harmony_sync_samples_));
            armed_recording_ = false;
            is_recording_ = true;
            loop_playing_ = keep_loop_playing;
            stretch_playing_ = keep_stretch_playing;
            prev_wraparound_count_ = playing_head_.GetWrapAroundCount();
        }
    }

    if (armed_stop_) {
        bool immediate_stop = false;
        if (midi_clock_running_) {
            if (clock_beat_) {
                immediate_stop = true;
                clock_beat_ = false;
            }
        } else {
            immediate_stop = true;
        }

        if (immediate_stop) {
            armed_stop_ = false;

            if (is_recording_) {
                if (is_stretching_) {
                    size_t source_length = (recording_layer_ == 0) ? loop_length_ : main_loop_length_;
                    if (source_length == 0) {
                        source_length = kMicroLoopMaxSize;
                    }
                    stretch_source_wrap_length_ = ComputeStretchSourceLength(stretch_slice_, source_length);
                }
                FinalizeRecording(true);
                is_recording_ = false;
                if (GetParameterAsBinnedValue(LOOP_MODE) != SAMPLER) {
                    loop_playing_ = true;
                }
            }
        }
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
                s_stage_progress = 0;
                s_stretch_state = StretchState::GATHER_FRAME;
            }
            return true;
        }

        switch(s_stretch_state) {
            case StretchState::IDLE:
                break;

            case StretchState::GATHER_FRAME:
                {
                    size_t buffer_len = stretch_source_wrap_length_;
                    if (is_recording_ && recording_layer_ == 0 && buffer_len > loop_length_) {
                        buffer_len = loop_length_;
                    }
                    size_t remaining = N - s_stage_progress;
                    size_t count = (remaining < kStretchWorkChunkSamples) ? remaining : kStretchWorkChunkSamples;
                    GatherFrameFromBuffer(s_snapshot_buffer, buffer_[0], buffer_len, stretch_read_pos_,
                                          s_stage_progress, count);
                    s_stage_progress += count;
                    if (s_stage_progress >= N) {
                        s_stage_progress = 0;
                        s_stretch_state = StretchState::APPLY_ANALYSIS_WINDOW;
                    }
                }
                break;

            case StretchState::APPLY_ANALYSIS_WINDOW:
                {
                    size_t remaining = N - s_stage_progress;
                    size_t count = (remaining < kStretchWorkChunkSamples) ? remaining : kStretchWorkChunkSamples;
                    size_t end = s_stage_progress + count;
                    for(size_t k = s_stage_progress; k < end; ++k) {
                        s_frame_time[k] = s_snapshot_buffer[k] * s_win[k];
                    }
                    s_stage_progress = end;
                    if (s_stage_progress >= N) {
                        s_stage_progress = 0;
                        s_stretch_state = StretchState::DO_FFT;
                    }
                }
                break;

            case StretchState::DO_FFT:
                s_fft.Direct(s_frame_time, s_frame_freq);
                s_stage_progress = 0;
                s_stretch_state = StretchState::EXTRACT_MAGNITUDES;
                break;

            case StretchState::EXTRACT_MAGNITUDES:
                {
                    if (s_stage_progress == 0) {
                        s_magnitudes[0] = fabsf(s_frame_freq[0]);
                        s_magnitudes[N/2 - 1] = fabsf(s_frame_freq[1]);
                        s_stage_progress = 1;
                    }
                    const size_t mag_end = (N / 2) - 1;
                    if (s_stage_progress < mag_end) {
                        size_t end = s_stage_progress + kStretchWorkChunkSamples;
                        if (end > mag_end) {
                            end = mag_end;
                        }
                        for(size_t k = s_stage_progress; k < end; ++k) {
                            float re = s_frame_freq[2*k];
                            float im = s_frame_freq[2*k + 1];
                            s_magnitudes[k] = sqrtf(re*re + im*im);
                        }
                        s_stage_progress = end;
                    }
                    if (s_stage_progress >= mag_end) {
                        s_stage_progress = 0;
                        s_stretch_state = StretchState::RANDOMIZE_PHASES;
                    }
                }
                break;

            case StretchState::RANDOMIZE_PHASES:
                {
                    if (s_stage_progress == 0) {
                        s_frame_freq[0] = s_magnitudes[0];
                        s_frame_freq[1] = s_magnitudes[N/2 - 1];
                        s_stage_progress = 1;
                    }
                    const size_t phase_end = (N / 2);
                    size_t end = s_stage_progress + kStretchWorkChunkSamples;
                    if (end > phase_end) {
                        end = phase_end;
                    }
                    for(size_t k = s_stage_progress; k < end; ++k) {
                        float mag = s_magnitudes[k];

                        float u, v, r2;
                        do {
                            u = s_rng.randSigned(-1.0f, 1.0f);
                            v = s_rng.randSigned(-1.0f, 1.0f);
                            r2 = u*u + v*v;
                        } while(r2 > 1.0f || r2 < 1e-12f);

                        float inv = mag / sqrtf(r2);
                        s_frame_freq[2*k] = u * inv;
                        s_frame_freq[2*k + 1] = v * inv;
                    }
                    s_stage_progress = end;
                    if (s_stage_progress >= phase_end) {
                        s_stage_progress = 0;
                        s_stretch_state = StretchState::DO_IFFT;
                    }
                }
                break;

            case StretchState::DO_IFFT:
                {
                    if (s_stage_progress == 0) {
                        s_fft.Inverse(s_frame_freq, s_frame_time);
                    }
                    const float scale = 1.0f / (float)N;
                    size_t remaining = N - s_stage_progress;
                    size_t count = (remaining < kStretchWorkChunkSamples) ? remaining : kStretchWorkChunkSamples;
                    size_t end = s_stage_progress + count;
                    for(size_t k = s_stage_progress; k < end; ++k) {
                        s_frame_time[k] *= scale;
                    }
                    s_stage_progress = end;
                    if (s_stage_progress >= N) {
                        s_stage_progress = 0;
                        s_stretch_state = StretchState::APPLY_SYNTHESIS_WINDOW;
                    }
                }
                break;

            case StretchState::APPLY_SYNTHESIS_WINDOW:
                {
                    size_t remaining = N - s_stage_progress;
                    size_t count = (remaining < kStretchWorkChunkSamples) ? remaining : kStretchWorkChunkSamples;
                    size_t end = s_stage_progress + count;
                    for(size_t k = s_stage_progress; k < end; ++k) {
                        s_result_buffer[k] = s_frame_time[k] * s_win[k];
                    }
                    s_stage_progress = end;
                    if (s_stage_progress >= N) {
                        s_stage_progress = 0;
                        s_stretch_state = StretchState::ADD_TO_OUTPUT;
                    }
                }
                break;

            case StretchState::ADD_TO_OUTPUT:
                {
                    bool can_add = (write_length > 0) && ((stretch_write_pos_ + N) <= write_length);
                    if (can_add) {
                        bool reverse_previous_frame = (s_synth_count % 2 == 1);
                        size_t remaining = N - s_stage_progress;
                        size_t count = (remaining < kStretchWorkChunkSamples) ? remaining : kStretchWorkChunkSamples;
                        OLA_AddFrame(write_buffer, write_norm, write_length, stretch_write_pos_,
                                     s_result_buffer, s_win2, reverse_previous_frame,
                                     s_stage_progress, count);
                        s_stage_progress += count;
                    } else {
                        s_stage_progress = N;
                    }

                    if (s_stage_progress >= N) {
                        s_stage_progress = 0;
                        if (can_add && (stretch_write_pos_ + H_OUT <= write_length)) {
                            stretch_write_pos_ += H_OUT;
                        }

                        s_synth_count++;
                        s_stretch_state = StretchState::CHECK_MORE_SYNTH;
                    }
                }
                break;

            case StretchState::CHECK_MORE_SYNTH:
                if(s_synth_count < STRETCH) {
                    // Switch once enough fully-overlapped samples exist (works even when STRETCH < 6).
                    size_t ready_len = 0;
                    if (stretch_write_pos_ > (N - H_OUT)) {
                        ready_len = stretch_write_pos_ - (N - H_OUT);
                    }
                    if (ready_len > 0) {
                        if (write_stretch_buffer_) {
                            stretched_ready_length_b_ = ready_len;
                            UpdateStreamingPlayRange(s_stretch_norm_b, stretched_ready_length_b_,
                                                     stretched_play_start_b_, stretched_play_length_b_,
                                                     stretched_play_locked_b_);
                        } else {
                            stretched_ready_length_a_ = ready_len;
                            UpdateStreamingPlayRange(s_stretch_norm_a, stretched_ready_length_a_,
                                                     stretched_play_start_a_, stretched_play_length_a_,
                                                     stretched_play_locked_a_);
                        }
                        bool play_ready = false;
                        if (write_stretch_buffer_) {
                            play_ready = (stretched_play_locked_b_
                                          && stretched_play_length_b_ >= kStretchMinPlayLength);
                        } else {
                            play_ready = (stretched_play_locked_a_
                                          && stretched_play_length_a_ >= kStretchMinPlayLength);
                        }
                        if (play_ready && active_stretch_buffer_ != write_stretch_buffer_) {
                            const bool was_using = use_stretched_buffer_;
                            if (was_using) {
                                stretch_swap_prev_buffer_ = active_stretch_buffer_;
                                stretch_swap_fade_count_ = stretch_swap_fade_samples_;
                                stretch_fade_in_count_ = 0;
                            } else {
                                stretch_swap_fade_count_ = 0;
                                const int mode = GetParameterAsBinnedValue(LOOP_MODE);
                                stretch_fade_in_count_ = ComputeStretchFadeInSamples(
                                    GetParameterAsFloat(ATTACK), mode);
                            }
                            active_stretch_buffer_ = write_stretch_buffer_;
                            use_stretched_buffer_ = true;
                            if (GetParameterAsBinnedValue(LOOP_MODE) != SAMPLER) {
                                stretch_playing_ = true;
                            }
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
                size_t available_samples = stretch_source_wrap_length_;
                if (is_recording_ && recording_layer_ == 0 && available_samples > loop_length_) {
                    available_samples = loop_length_;
                }

                // Calculate next read position
                size_t next_read_pos = stretch_read_pos_ + H_IN;

                // Check if we have enough samples for a full frame
                bool next_frame_available = (next_read_pos + N <= available_samples);
                // Stretch source is layer 0, so overdub on layer 1 should not
                // block stretch completion.
                bool recording_done = !(is_recording_ && recording_layer_ == 0);

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

                    bool write_buffer_is_active = use_stretched_buffer_
                                                  && (active_stretch_buffer_ == write_stretch_buffer_);
                    if (final_length > 0 && !write_buffer_is_active) {
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
                size_t final_length = write_stretch_buffer_ ? stretched_length_b_ : stretched_length_a_;
                bool write_buffer_is_active = use_stretched_buffer_
                                              && (active_stretch_buffer_ == write_stretch_buffer_);

                if (final_length > 0) {
                    if (write_buffer_is_active) {
                        if (write_stretch_buffer_) {
                            UpdateStreamingPlayRange(write_norm, final_length,
                                                     stretched_play_start_b_, stretched_play_length_b_,
                                                     stretched_play_locked_b_);
                        } else {
                            UpdateStreamingPlayRange(write_norm, final_length,
                                                     stretched_play_start_a_, stretched_play_length_a_,
                                                     stretched_play_locked_a_);
                        }
                    } else {
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
                }

                if (final_length > 0 && !write_buffer_is_active
                    && active_stretch_buffer_ != write_stretch_buffer_) {
                    const bool was_using = use_stretched_buffer_;
                    if (was_using) {
                        stretch_swap_prev_buffer_ = active_stretch_buffer_;
                        stretch_swap_fade_count_ = stretch_swap_fade_samples_;
                        stretch_fade_in_count_ = 0;
                    } else {
                        stretch_swap_fade_count_ = 0;
                        const int mode = GetParameterAsBinnedValue(LOOP_MODE);
                        stretch_fade_in_count_ = ComputeStretchFadeInSamples(
                            GetParameterAsFloat(ATTACK), mode);
                    }
                    active_stretch_buffer_ = write_stretch_buffer_;
                    use_stretched_buffer_ = true;
                    if (GetParameterAsBinnedValue(LOOP_MODE) != SAMPLER) {
                        stretch_playing_ = true;
                    }
                }

                is_stretching_ = false;
                s_stretch_state = StretchState::IDLE;
                break;
            }
        }
    }
    return true;
}

bool MicroLooperModule::IsRecording() const {
    return is_recording_;
}

float MicroLooperModule::GetBrightnessForLED(int led_id) const
{
    const uint32_t now_ms = daisy::System::GetNow();

    if (led_id == 0 && midi_clock_running_ && (armed_recording_ || armed_stop_)) {
        bool on = ((now_ms / kMidiSyncWaitBlinkMs) % 2) == 0;
        return on ? 1.0f : 0.0f;
    }

    const bool no_recorded_loop = !is_recording_ && !has_committed_loop_ && (main_loop_length_ == 0);
    if (led_id == 0 && midi_clock_running_ && no_recorded_loop && now_ms < midi_pulse_flash_until_ms_) {
        return 1.0f;
    }

    int mode = GetParameterAsBinnedValue(LOOP_MODE);
    if (mode == SAMPLER) {
        if (led_id == 0) {
            return loop_playing_ ? 1.0f : 0.0f;
        } else {
            return stretch_playing_ ? 1.0f : 0.0f;
        }
    } else {
        if (led_id == 0) {
            if (is_recording_) {
                return (now_ms < record_led_blink_until_ms_) ? 0.0f : 1.0f;
            }
            return (now_ms < record_led_blink_until_ms_) ? 1.0f : 0.0f;
        } else {
            if (is_stretching_) {
                bool on = ((now_ms / kRecordLedWrapBlinkMs) % 2) == 0;
                return on ? 1.0f : 0.0f;
            }
            return (stretch_playing_ && use_stretched_buffer_) ? 1.0f : 0.0f;
        }
    }
}
