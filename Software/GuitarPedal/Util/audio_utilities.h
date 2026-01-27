#pragma once
#ifndef AUDIO_UTILITIES_H
#define AUDIO_UTILITIES_H

#include <stdint.h>

/**
 * \file audio_utilities.h
 * \brief Audio utility functions for tempo/time conversion and crossfading.
 */

/**
 * \brief Convert tempo (BPM) to frequency (Hz).
 * \param tempo_bpm Tempo in beats per minute (BPM).
 * \return Frequency in cycles per second (Hz).
 */
inline float tempo_to_freq(uint32_t tempo_bpm)
{
    return static_cast<float>(tempo_bpm) / 60.0f;
}

/**
 * \brief Convert frequency (Hz) to tempo (BPM).
 * \param freq_hz Frequency in cycles per second (Hz).
 * \return Tempo in beats per minute (BPM).
 *
 * \note Result is truncated to an integer BPM.
 */
inline uint32_t freq_to_tempo(float freq_hz)
{
    return static_cast<uint32_t>(freq_hz * 60.0f);
}

/**
 * \brief Convert milliseconds per beat to tempo (BPM).
 * \param ms_per_beat Time per beat in milliseconds.
 * \return Tempo in beats per minute (BPM).
 *
 * \warning Undefined for ms_per_beat == 0.
 */
inline uint32_t ms_to_tempo(uint32_t ms_per_beat)
{
    return 60000u / ms_per_beat;
}

/**
 * \brief Convert seconds per beat to tempo (BPM).
 * \param seconds_per_beat Time per beat in seconds.
 * \return Tempo in beats per minute (BPM).
 *
 * \warning Undefined for seconds_per_beat == 0.
 */
inline uint32_t s_to_tempo(float seconds_per_beat)
{
    return static_cast<uint32_t>(60.0f / seconds_per_beat);
}

/**
 * \brief Wet/dry gain coefficients for a crossfade.
 */
struct CrossfadeGains
{
    float wet; /**< Wet gain coefficient. */
    float dry; /**< Dry gain coefficient. */
};

/**
 * \brief Compute an approximately energy-constant wet/dry crossfade.
 *
 * Implements the SignalSmith cheap energy crossfade:
 * https://signalsmith-audio.co.uk/writing/2021/cheap-energy-crossfade/
 *
 * \param mix Dry/wet mix in [0, 1].
 * \return Wet and dry gain coefficients.
 *
 * \note Input is not clamped.
 */
inline CrossfadeGains EnergyCrossfade(float mix)
{
    float x2 = 1.f - mix;
    float A  = mix * x2;
    float B  = A * (1.f + 1.4186f * A);
    float C  = B + mix;
    float D  = B + x2;

    return { C * C, D * D };
}

#endif // AUDIO_UTILITIES_H
