#include "playing_head.h"
#include "audio_utilities.h"
#include "XorShift32.h"

namespace {
XorShift32 s_playing_head_rng;
}

void PlayingHead::Reset() {
    head_position_f_ = 0.0f;
    wrap_around_count_ = 0;
    pingpong_dir_ = 1.0f;
    sync_scale_ = 0.0f;
    sync_total_ = 0;
    sync_remaining_ = 0;
}

static inline float wrapf(float x, float L)
{
    float r = std::fmod(x, L);
    if (r < 0.0f) {
        r += L;
    }
    if (r == 0.0f && x != 0.0f) {
        return L;         // special case: wrap L → L instead of 0
    }
    return r;
}

static inline float wrap_pos(float x, float L)
{
    float r = std::fmod(x, L);
    if (r < 0.0f) {
        r += L;
    }
    return r;
}

void PlayingHead::UpdatePosition(size_t loop_length, float slice, float start_pos) {
    // When recording the first layer, if recording with negative speed, 
    // we still write the buffer in forward direction.
    // The buffer will be reversed when the "stop recording" button is pressed.
    // This is done becausse we do not know how long the recording will be.
    
    // Compute slice length
    float slice_length = static_cast<float>(loop_length) * slice;

    float sync_offset = 0.0f;
    if (sync_remaining_ > 0 && sync_total_ > 1) {
        uint32_t idx = sync_total_ - sync_remaining_;
        float t = static_cast<float>(idx) / static_cast<float>(sync_total_ - 1);
        float w = HannWeight(t);
        sync_offset = sync_scale_ * w;
    }
    float speed = speed_ + sync_offset;

    // Compute slice end
    float end_pos;
    if (speed > 0.0f) {
        end_pos = wrapf(start_pos + slice_length, static_cast<float>(loop_length));
    } else {
        end_pos = wrapf(start_pos - slice_length, static_cast<float>(loop_length));
    }

    // Advance playback head
    head_position_f_ += speed;

    // Forward playback
    if (speed > 0.0f) {
        if (start_pos < end_pos) {
            // contiguous slice
            if (head_position_f_ >= end_pos) {
                head_position_f_ = start_pos + std::fmod(head_position_f_ - end_pos, slice_length);
                wrap_around_count_++;
            }
        } else {
            // wrapped slice
            if (head_position_f_ >= static_cast<float>(loop_length)) {
                head_position_f_ -= static_cast<float>(loop_length);
            }
            if (head_position_f_ >= end_pos && head_position_f_ < start_pos) {
                head_position_f_ = start_pos + std::fmod(head_position_f_ - end_pos, slice_length);
                wrap_around_count_++;
            }
        }
    }

    // Backward playback
    else if (speed < 0.0f) {
        if (start_pos < end_pos) {
            // contiguous slice
            if (head_position_f_ < start_pos) {
                head_position_f_ = end_pos - std::fmod(start_pos - head_position_f_, slice_length);
                wrap_around_count_++;
            }
        } else {
            // wrapped slice
            if (head_position_f_ < 0.0f) {
                head_position_f_ += static_cast<float>(loop_length);
            }
            if (head_position_f_ < start_pos && head_position_f_ >= end_pos) {
                head_position_f_ = end_pos - std::fmod(start_pos - head_position_f_, slice_length);
            }
        }
    }

    // (speed == 0) → head_position_f_ stays still

    if (sync_remaining_ > 0) {
        sync_remaining_--;
        if (sync_remaining_ == 0) {
            sync_scale_ = 0.0f;
            sync_total_ = 0;
        }
    }
}

bool PlayingHead::UpdatePositionRandomBounce(size_t loop_length,
                                             float bounce_probability) {
    if (loop_length < 2) {
        head_position_f_ = 0.0f;
        return false;
    }
    float sync_offset = 0.0f;
    if (sync_remaining_ > 0 && sync_total_ > 1) {
        uint32_t idx = sync_total_ - sync_remaining_;
        float t = static_cast<float>(idx) / static_cast<float>(sync_total_ - 1);
        float w = HannWeight(t);
        sync_offset = sync_scale_ * w;
    }
    float speed = speed_ + sync_offset;
    float step = std::fabs(speed);
    if (step == 0.0f) {
        return false;
    }

    if (bounce_probability < 0.0f) {
        bounce_probability = 0.0f;
    } else if (bounce_probability > 1.0f) {
        bounce_probability = 1.0f;
    }

    float random01 = (s_playing_head_rng.nextU32() >> 8) * (1.0f / 16777216.0f);

    float max_pos = static_cast<float>(loop_length - 1);
    float next = head_position_f_ + (step * pingpong_dir_);
    bool boundary_hit = false;

    if (pingpong_dir_ > 0.0f) {
        if (next > max_pos) {
            float overshoot = next - max_pos;
            if (random01 < bounce_probability) {
                head_position_f_ = max_pos - overshoot;
                pingpong_dir_ = -pingpong_dir_;
            } else {
                head_position_f_ = overshoot;
            }
            wrap_around_count_++;
            boundary_hit = true;
        }
    } else {
        if (next < 0.0f) {
            float overshoot = -next;
            if (random01 < bounce_probability) {
                head_position_f_ = overshoot;
                pingpong_dir_ = -pingpong_dir_;
            } else {
                head_position_f_ = max_pos - overshoot;
            }
            wrap_around_count_++;
            boundary_hit = true;
        }
    }

    if (!boundary_hit) {
        head_position_f_ = next;
    }
    if (sync_remaining_ > 0) {
        sync_remaining_--;
        if (sync_remaining_ == 0) {
            sync_scale_ = 0.0f;
            sync_total_ = 0;
        }
    }
    return boundary_hit;
}

bool PlayingHead::SyncTo(const PlayingHead& target, size_t loop_length, float sync_samples) {
    if (loop_length == 0 || sync_samples <= 0.0f) {
        head_position_f_ = target.head_position_f_;
        sync_scale_ = 0.0f;
        sync_total_ = 0;
        sync_remaining_ = 0;
        return true;
    }

    if (sync_samples < 1.0f) {
        head_position_f_ = target.head_position_f_;
        sync_scale_ = 0.0f;
        sync_total_ = 0;
        sync_remaining_ = 0;
        return true;
    }

    float loop_len_f = static_cast<float>(loop_length);
    float current = wrap_pos(head_position_f_, loop_len_f);
    float target_pos = wrap_pos(target.head_position_f_, loop_len_f);

    float delta = target_pos - current;
    if (delta > (loop_len_f * 0.5f)) {
        delta -= loop_len_f;
    } else if (delta < -(loop_len_f * 0.5f)) {
        delta += loop_len_f;
    }

    uint32_t steps = static_cast<uint32_t>(std::ceil(sync_samples));
    if (steps == 0) {
        head_position_f_ = target.head_position_f_;
        sync_scale_ = 0.0f;
        sync_total_ = 0;
        sync_remaining_ = 0;
        return true;
    }

    head_position_f_ = current;
    float sum_w = 0.0f;
    if (steps > 1) {
        for (uint32_t i = 0; i < steps; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(steps - 1);
            sum_w += HannWeight(t);
        }
    }

    if (sum_w <= 0.0f) {
        head_position_f_ = target.head_position_f_;
        sync_scale_ = 0.0f;
        sync_total_ = 0;
        sync_remaining_ = 0;
        return true;
    }

    sync_scale_ = delta / sum_w;
    sync_total_ = steps;
    sync_remaining_ = steps;
    return true;
}
