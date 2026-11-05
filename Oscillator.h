// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>

/**
    @file Oscillator.h

    Band-limited oscillators for one synth voice: PolyBlepOscillator (sine,
    square, triangle and sawtooth, with PolyBLEP residuals on the square and
    saw steps) and RandomStepOscillator (the Random source).

    A waveform change waits for the next cycle boundary, so switching shapes
    while a note sounds never clicks. The phase is kept across notes, so a
    retriggered or stolen voice continues smoothly.

    Audio thread only; no allocation.
*/
class PolyBlepOscillator
{
public:
    enum Waveform { sine = 0, square = 1, triangle = 2, sawtooth = 3 };

    /** Takes effect at the next cycle boundary. */
    void setWaveform (int newWaveform) noexcept   { pendingWaveform = newWaveform; }

    /** Sets the pitch immediately (no glide). */
    void setFrequency (float hz, double sampleRate) noexcept
    {
        phaseInc = (float) (hz / sampleRate);
    }

    int getWaveform() const noexcept   { return waveform; }

    float nextSample() noexcept
    {
        const float t  = phase;
        const float dt = phaseInc;
        float value;

        switch (waveform)
        {
            case square:
                value = (t < 0.5f ? 1.0f : -1.0f)
                      + polyBlep (t, dt)
                      - polyBlep (std::fmod (t + 0.5f, 1.0f), dt);
                break;
            case triangle:   // continuous, so naive is fine
                value = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
                break;
            case sawtooth:
                value = 2.0f * t - 1.0f - polyBlep (t, dt);
                break;
            default:         // sine
                value = std::sin (juce::MathConstants<float>::twoPi * t);
                break;
        }

        phase += dt;
        if (phase >= 1.0f)
        {
            phase -= 1.0f;
            waveform = pendingWaveform;   // switch shapes only at the cycle boundary
        }
        return value;
    }

private:
    /** PolyBLEP residual: removes most aliasing from a step discontinuity. */
    static float polyBlep (float t, float dt) noexcept
    {
        if (t < dt)
        {
            t /= dt;
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt)
        {
            t = (t - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

    float phase = 0.0f, phaseInc = 0.0f;
    int   waveform = sine, pendingWaveform = sine;
};

/**
    The Random source: a pitched wave of 16 random steps per cycle that keeps
    evolving.

    Each cycle is 16 equal steps, each holding a level drawn uniformly from
    -1..1. About once per cycle (every 17 steps) one level is replaced by a
    new random value, so the wave mostly repeats from cycle to cycle (that
    repetition is what gives it a pitch) while its timbre drifts: the whole
    shape is renewed every 17 cycles. Re-rolling every level every cycle
    would repeat nothing, which sounds like sample-and-hold noise with no
    pitch at the note's frequency.

    The replaced level is always the one 8 steps away from the playhead, so
    neither of the two step edges next to the playhead ever changes while
    they are being band-limited.

    Each step edge gets a PolyBLEP residual scaled by its height, the same
    correction the square and saw edges get. It is exact while a step lasts
    at least two samples (up to 1/32 of the sample rate: 1.5 kHz at 48 kHz,
    about G6); above that the residual is capped, so the output stays within
    -1..1 but aliases more.

    Seeded: the same seed and the same notes render the same audio. The phase
    is kept across notes like the oscillator's, so a retrigger never clicks.

    Audio thread only; no allocation.
*/
class RandomStepOscillator
{
public:
    static constexpr int numSteps = 16;

    /** Restarts the random stream and the phase (prepare, reset). */
    void reseed (juce::uint32 seed) noexcept
    {
        state = seed != 0 ? seed : 0x9e3779b9u;
        for (int i = 0; i < 8; ++i)   // warm up: nearby seeds give unrelated streams
            nextRandom();
        for (auto& l : levels)
            l = nextRandom();
        phase = 0.0f;
        lastStep = 0;
        stepsSinceReroll = 0;
    }

    /** Sets the pitch immediately (no glide). */
    void setFrequency (float hz, double sampleRate) noexcept
    {
        phaseInc = (float) (hz / sampleRate);
    }

    float nextSample() noexcept
    {
        const float s     = phase * (float) numSteps;
        const int   step  = juce::jmin ((int) s, numSteps - 1);
        const float local = s - (float) step;                                // 0..1 within the step
        const float dt    = juce::jmin (phaseInc * (float) numSteps, 0.5f);  // step length in samples, as a fraction; capped

        if (step != lastStep)
            advanceTo (step);

        const float current = levels[(size_t) step];
        float value = current;

        // Just after the edge into this step, or just before the edge out of it.
        if (local < dt)
        {
            const float x = local / dt;
            value += 0.5f * (current - levels[(size_t) ((step + numSteps - 1) % numSteps)]) * (x + x - x * x - 1.0f);
        }
        else if (local > 1.0f - dt)
        {
            const float x = (local - 1.0f) / dt;
            value += 0.5f * (levels[(size_t) ((step + 1) % numSteps)] - current) * (x * x + x + x + 1.0f);
        }

        phase += phaseInc;
        if (phase >= 1.0f)
            phase -= 1.0f;
        return value;
    }

    /** The 16 levels of the current cycle (tests and visualisers). */
    const std::array<float, numSteps>& getLevels() const noexcept   { return levels; }

private:
    /** Counts the steps entered since the last re-roll; every 17th re-rolls the level 8 steps ahead. */
    void advanceTo (int step) noexcept
    {
        stepsSinceReroll += (step - lastStep + numSteps) % numSteps;
        lastStep = step;
        if (stepsSinceReroll >= numSteps + 1)
        {
            stepsSinceReroll = 0;
            levels[(size_t) ((step + numSteps / 2) % numSteps)] = nextRandom();
        }
    }

    /** xorshift32, mapped to -1..1. */
    float nextRandom() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) (juce::int32) state * (1.0f / 2147483648.0f);
    }

    std::array<float, numSteps> levels {};
    juce::uint32 state = 0x9e3779b9u;
    float phase = 0.0f, phaseInc = 0.0f;
    int   lastStep = 0, stepsSinceReroll = 0;
};
