#pragma once
#ifndef XORSHIFT32_H
#define XORSHIFT32_H

#include <stdint.h>

namespace bkshepherd {

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

} // namespace bkshepherd

#endif
