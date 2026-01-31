#include "playing_head.h"

void PlayingHead::Reset() {
    head_position_f_ = 0.0f;
    wrap_around_count_ = 0;
    pingpong_dir_ = 1.0f;
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

void PlayingHead::UpdatePosition(size_t loop_length, float slice, float start_pos) {
    // When recording the first layer, if recording with negative speed, 
    // we still write the buffer in forward direction.
    // The buffer will be reversed when the "stop recording" button is pressed.
    // This is done becausse we do not know how long the recording will be.
    
    // Compute slice length
    float slice_length = static_cast<float>(loop_length) * slice;

    // Compute slice end
    float end_pos;
    if (speed_ > 0.0f) {
        end_pos = wrapf(start_pos + slice_length, static_cast<float>(loop_length));
    } else {
        end_pos = wrapf(start_pos - slice_length, static_cast<float>(loop_length));
    }

    // Advance playback head
    head_position_f_ += speed_;

    // Forward playback
    if (speed_ > 0.0f) {
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
    else if (speed_ < 0.0f) {
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
}

bool PlayingHead::UpdatePositionPingPong(size_t loop_length) {
    if (loop_length < 2) {
        head_position_f_ = 0.0f;
        return false;
    }
    float step = std::fabs(speed_);
    if (step == 0.0f) {
        return false;
    }

    float max_pos = static_cast<float>(loop_length - 1);
    float next = head_position_f_ + (step * pingpong_dir_);

    if (pingpong_dir_ > 0.0f) {
        if (next > max_pos) {
            float overshoot = next - max_pos;
            head_position_f_ = max_pos - overshoot;
            pingpong_dir_ = -pingpong_dir_;
            wrap_around_count_++;
            return true;
        }
    } else {
        if (next < 0.0f) {
            float overshoot = -next;
            head_position_f_ = overshoot;
            pingpong_dir_ = -pingpong_dir_;
            wrap_around_count_++;
            return true;
        }
    }

    head_position_f_ = next;
    return false;
}

bool PlayingHead::SyncTo(const PlayingHead& target) {
    head_position_f_ = target.head_position_f_;
    return true;
}
