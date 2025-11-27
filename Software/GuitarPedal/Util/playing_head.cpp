#include "playing_head.h"

void PlayingHead::Reset() {
    head_position_f_ = 0.0f;
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

void PlayingHead::UpdatePosition(bool first_layer, size_t loop_length, float slice, float start_pos) {
    // When recording the first layer, if recording with negative speed, 
    // we still write the buffer in forward direction.
    // The buffer will be reversed when the "stop recording" button is pressed.
    // This is done because we do not know how long the recording will be.
    float speed;
    if (first_layer && speed_ < 0.0f) {
        speed = -speed_;
    } else {
        speed = speed_;
    } 
    
    // Compute slice length
    float slice_length = static_cast<float>(loop_length) * slice;

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
}

bool PlayingHead::SyncTo(const PlayingHead& target) {
    head_position_f_ = target.head_position_f_;
    return true;
}