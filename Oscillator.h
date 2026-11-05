// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

/**
    @file Oscillator.h

    Band-limited oscillator for one synth voice: sine, square, triangle and
    sawtooth, with PolyBLEP residuals on the square and saw steps.

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
