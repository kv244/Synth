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
    square, triangle and sawtooth) and RandomStepOscillator (the Random
    source), sharing the residuals in Blep4.

    Jumps (the square and saw edges, Random's steps) get a 4-point BLEP
    residual, and the triangle's corners a 4-point BLAMP residual. Both come
    from the cubic B-spline: it spreads each jump or corner over two samples
    either side, which removes more of the aliasing near Nyquist than the
    2-point PolyBLEP used before v1.6.0.

    A waveform change waits for the next cycle boundary, so switching shapes
    while a note sounds never clicks. The phase is kept across notes, so a
    retriggered or stolen voice continues smoothly.

    Audio thread only; no allocation.
*/
/**
    4-point band-limiting residuals from the cubic B-spline.

    A jump of height h at the start of a cycle is corrected by adding
    h * step (t, dt) to the naive wave; a corner where the slope (per sample)
    changes by k is corrected by adding k * corner (t, dt). t is the phase
    (0..1) and dt the phase step per sample.

    Each residual reaches two samples either side of the edge. That is exact
    while dt <= 0.25 (an edge's reach is then shorter than a cycle, so only the
    edges at the start of this cycle and the next can be in range); above it,
    up to the 0.49 cap, some aliasing returns but the output stays bounded.
*/
struct Blep4
{
    /** Integral of the B-spline from d to 2 (d = samples from the edge, 0..2): 0.5 at the edge, 0 at 2. */
    static float tail (float d) noexcept
    {
        if (d >= 1.0f)
        {
            const float u = 2.0f - d, u2 = u * u;
            return u2 * u2 * (1.0f / 24.0f);
        }
        const float d2 = d * d;
        return 0.5f - (4.0f * d - 2.0f * d2 * d + 0.75f * d2 * d2) * (1.0f / 6.0f);
    }

    /** Integral of tail() from d to 2: the corner residual's shape, 7/30 at the corner. */
    static float tail2 (float d) noexcept
    {
        if (d >= 1.0f)
        {
            const float u = 2.0f - d, u2 = u * u;
            return u2 * u2 * u * (1.0f / 120.0f);
        }
        const float d2 = d * d, d4 = d2 * d2;
        return 1.0f / 120.0f + 0.5f * (1.0f - d) - (1.65f - 2.0f * d2 + 0.5f * d4 - 0.15f * d4 * d) * (1.0f / 6.0f);
    }

    /** Residual for a unit upward jump at phase 0 (repeating every cycle). */
    static float step (float t, float dt) noexcept
    {
        float r = 0.0f;
        if (t < 2.0f * dt)          r -= tail (t / dt);            // just after the jump
        if (t > 1.0f - 2.0f * dt)   r += tail ((1.0f - t) / dt);   // just before the next one
        return r;
    }

    /** Residual for a corner at phase 0 whose slope (per sample) rises by 1. */
    static float corner (float t, float dt) noexcept
    {
        float r = 0.0f;
        if (t < 2.0f * dt)          r += tail2 (t / dt);
        if (t > 1.0f - 2.0f * dt)   r += tail2 ((1.0f - t) / dt);
        return r;
    }

    /** Phase steps at or above this are clamped: band-limiting needs at least two samples a cycle. */
    static constexpr float maxPhaseStep = 0.49f;

    /**
        Treble compensation for the B-spline residuals.

        Spreading every edge with the cubic B-spline acts like a fixed low-pass on
        the whole wave: its harmonics are scaled by sinc^4 (f / sampleRate), about
        -2.8 dB at 10 kHz and -6.3 dB at 16 kHz at 48 kHz (twice the old 2-point
        residual's loss). This 3-tap filter, y[n] = (1 + 2a) x[n-1] - a (x[n] + x[n-2]),
        undoes most of it: harmonics stay within 0.5 dB of the ideal wave up to a
        quarter of the sample rate. It boosts the leftover aliases too, but they
        stay 4-5 dB below the 2-point residual's. The cost is three multiplies and
        one sample of latency (none for the sine, which bypasses it).
    */
    struct TrebleCompensation
    {
        static constexpr float a = 0.27f;   // matches 1 / sinc^4 at a quarter of the sample rate

        float process (float x) noexcept
        {
            const float y = (1.0f + 2.0f * a) * x1 - a * (x + x2);
            x2 = x1;
            x1 = x;
            return y;
        }

        /**
            For the sine, which has no edges to compensate: passes x straight through
            (so sine renders are unchanged from v1.5.0) and only keeps the history,
            so a switch to another shape at the cycle boundary starts from real samples.
        */
        float bypass (float x) noexcept
        {
            x2 = x1;
            x1 = x;
            return x;
        }

        float x1 = 0.0f, x2 = 0.0f;
    };
};

class PolyBlepOscillator
{
public:
    enum Waveform { sine = 0, square = 1, triangle = 2, sawtooth = 3 };

    /** Takes effect at the next cycle boundary. */
    void setWaveform (int newWaveform) noexcept   { pendingWaveform = newWaveform; }

    /** Sets the pitch immediately (no glide). Clamped below half the sample rate. */
    void setFrequency (float hz, double sampleRate) noexcept
    {
        phaseInc = juce::jmin ((float) (hz / sampleRate), Blep4::maxPhaseStep);
    }

    int getWaveform() const noexcept   { return waveform; }

    float nextSample() noexcept
    {
        const float t  = phase;
        const float dt = phaseInc;
        float value;

        switch (waveform)
        {
            case square:     // +2 at phase 0, -2 at phase 0.5
            {
                const float half = t < 0.5f ? t + 0.5f : t - 0.5f;   // the phase seen from the second edge
                value = (t < 0.5f ? 1.0f : -1.0f)
                      + 2.0f * (Blep4::step (t, dt) - Blep4::step (half, dt));
                break;
            }
            case triangle:   // no jumps, but corners: the slope changes by +8 / -8 per cycle at phase 0 / 0.5
            {
                const float half = t < 0.5f ? t + 0.5f : t - 0.5f;
                value = (t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t)
                      + 8.0f * dt * (Blep4::corner (t, dt) - Blep4::corner (half, dt));
                break;
            }
            case sawtooth:   // -2 at phase 0
                value = 2.0f * t - 1.0f - 2.0f * Blep4::step (t, dt);
                break;
            default:         // sine
                value = std::sin (juce::MathConstants<float>::twoPi * t);
                break;
        }

        const float out = waveform == sine ? treble.bypass (value) : treble.process (value);

        phase += dt;
        if (phase >= 1.0f)
        {
            phase -= 1.0f;
            waveform = pendingWaveform;   // switch shapes only at the cycle boundary
        }
        return out;
    }

private:
    Blep4::TrebleCompensation treble;
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

    Each step edge gets the 4-point residual (Blep4) scaled by its height, the
    same correction the square and saw edges get. It is exact while a step
    lasts at least four samples (up to 1/64 of the sample rate: 750 Hz at
    48 kHz); shorter steps get overlapping corrections, then a capped one, so
    the output stays within -1..1 but aliases more.

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
        phaseInc = juce::jmin ((float) (hz / sampleRate), Blep4::maxPhaseStep);
    }

    float nextSample() noexcept
    {
        const float s     = phase * (float) numSteps;
        const int   step  = juce::jmin ((int) s, numSteps - 1);
        const float local = s - (float) step;                                // 0..1 within the step
        const float dt    = juce::jmin (phaseInc * (float) numSteps, 0.5f);  // phase step within a step; capped

        if (step != lastStep)
            advanceTo (step);

        const float current = levels[(size_t) step];
        float value = current;

        // Within two samples after the edge into this step, or before the edge out of it
        // (both, when a step is shorter than four samples).
        if (local < 2.0f * dt)
            value -= (current - levels[(size_t) ((step + numSteps - 1) % numSteps)]) * Blep4::tail (local / dt);
        if (local > 1.0f - 2.0f * dt)
            value += (levels[(size_t) ((step + 1) % numSteps)] - current) * Blep4::tail ((1.0f - local) / dt);

        phase += phaseInc;
        if (phase >= 1.0f)
            phase -= 1.0f;
        return value;   // no treble compensation: at high notes Random's steps run far above Nyquist, and the boost would amplify them
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
