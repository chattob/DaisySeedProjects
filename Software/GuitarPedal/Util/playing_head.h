#pragma once

#include <cstddef>   // for size_t
#include <cstdint>   // for uint16_t, uint32_t, etc.
#include <cmath>     // for std::fmod

class PlayingHead {
    public:
        void Reset();
        inline void SetSpeed(float speed) {speed_ = speed;};
        void UpdatePosition(bool first_layer, size_t loop_length, float slice = 1.0f, float start_pos = 0.0f);
        inline float GetHeadPosition() {return head_position_f_;};
        inline float GetWrapAroundCount() {return wrap_around_count_;};
        bool SyncTo(const PlayingHead& target);

    private:
        float head_position_f_ = 0.0f;
        float speed_ = 1.0f;
        uint16_t wrap_around_count_ = 0;
};