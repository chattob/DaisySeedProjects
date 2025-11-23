#pragma once

static constexpr float kSampleRate = 48000.f;
static constexpr size_t kMaxBufferSize = (static_cast<int>(kSampleRate) * 30); // 30 seconds of floats at 48 khz
static constexpr size_t kNumLayers = 7;