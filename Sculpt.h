// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <cmath>

/**
    @file Sculpt.h

    The two stages after CHOP for the Oscillator and Random sources (v1.6.0):
    Noise, then Fold. Both run per voice, per sample, before the envelope.

    Each has a neutral setting that leaves the sound untouched, bit for bit:
    Noise at 0% and Fold's Mix at 0%. With both there and nothing gliding,
    process() returns its input, so sessions from before v1.6.0 sound the same.

    Audio thread only; no allocation.
*/
struct SculptSettings
{
    float noise    = 0.0f;   ///< 0..1: equal-power crossfade from the wave to white noise
    float foldDrive = 3.0f;  ///< 1..10
    float foldMix  = 0.0f;   ///< 0..1: dry/wet; 0 skips the fold
};

class Sculpt
{
public:
    void prepare (double newSampleRate) noexcept
    {
        noise.reset (newSampleRate, 0.01);
        drive.reset (newSampleRate, 0.01);
        mix.reset (newSampleRate, 0.01);
        engage.reset (newSampleRate, 0.01);
        noise.setCurrentAndTargetValue (pending.noise);
        drive.setCurrentAndTargetValue (pending.foldDrive);
        mix.setCurrentAndTargetValue (pending.foldMix);
        engage.setCurrentAndTargetValue (pending.foldMix > 0.0f ? 1.0f : 0.0f);
        // One-pole DC blocker at about 10 Hz.
        dcCoefficient = 1.0f - juce::MathConstants<float>::twoPi * 10.0f / (float) newSampleRate;
        dcIn = dcOut = dryIn = dryOut = 0.0f;
        input.fill (0.0f);
        foldedEven.fill (0.0f);
        foldedOdd.fill (0.0f);
        noiseGainsFor = -1.0f;
    }

    void set (const SculptSettings& s) noexcept
    {
        pending = { juce::jlimit (0.0f, 1.0f, s.noise), juce::jlimit (1.0f, 10.0f, s.foldDrive), juce::jlimit (0.0f, 1.0f, s.foldMix) };
        noise.setTargetValue (pending.noise);
        drive.setTargetValue (pending.foldDrive);
        mix.setTargetValue (pending.foldMix);
        engage.setTargetValue (pending.foldMix > 0.0f ? 1.0f : 0.0f);
    }

    /** Restarts the noise generator (prepare, reset): the same seed and notes render the same audio. */
    void reseed (juce::uint32 seed) noexcept
    {
        state = seed != 0 ? seed : 0x2545f491u;
        for (int i = 0; i < 8; ++i)
            nextNoise();
    }

    float process (float x) noexcept
    {
        // Noise: equal-power crossfade, so the level holds steady across the knob (the
        // noise, uniform in -1..1, has the sawtooth's RMS of 0.577).
        if (noise.isSmoothing() || noise.getTargetValue() > 0.0f)
        {
            const float amount = noise.getNextValue();
            if (amount != noiseGainsFor)   // the trig only while the knob moves
            {
                noiseGainsFor = amount;
                waveGain  = std::cos (amount * juce::MathConstants<float>::halfPi);
                noiseGain = std::sin (amount * juce::MathConstants<float>::halfPi);
            }
            x = x * waveGain + nextNoise() * noiseGain;
        }

        // Fold. The input history is kept even while the fold is off, so it starts from real samples.
        if (! engage.isSmoothing() && engage.getTargetValue() <= 0.0f)
        {
            pushInput (x);
            return x;   // off: no delay, no change
        }

        // 2x oversampling: two samples at twice the rate from the half-band filter (the second
        // phase is just the input delayed 7 samples), each folded, then filtered and decimated
        // back. The folded signal comes out 14.5 samples late (the two filters). Antiderivative
        // anti-aliasing (ADAA) was tried on top: once oversampled it gained under 1 dB, for
        // twice the sine work, so the oversampled path folds the sine directly.
        // The newest sample of each filter is used from a register and only pushed into its history
        // afterwards: reading a value back from memory right after writing it, as part of a wider
        // (vectorised) load, stalls the CPU on store forwarding, which tripled this stage's cost.
        const float c = drive.getNextValue();
        const float* older = recent (input, inputPos);   // older[0] is the previous input, older[14] fifteen samples back
        const float upEven = upTaps[0] * x + dot15 (upTaps.data() + 1, older);
        const float upOdd = older[6];                    // the input 7 samples back

        const float foldedE = fastSin (juce::MathConstants<float>::halfPi * c * upEven);
        const float foldedO = fastSin (juce::MathConstants<float>::halfPi * c * upOdd);
        const float down = 0.5f * recent (foldedEven, foldedPos)[6]   // the even phase 7 samples back
                         + downTaps[0] * foldedO + dot15 (downTaps.data() + 1, recent (foldedOdd, foldedPos));
        pushFolded (foldedE, foldedO);
        const float wet = removeDc (down);

        // The dry signal, delayed by the same 14.5 samples so a partial Mix doesn't comb-filter:
        // 14 whole samples, then a first-order allpass for the last 0.5 (flat in level, unlike
        // interpolating between two samples, which would dull the dry treble by 3 dB at 12 kHz).
        const float delayed = older[13];   // the input 14 samples back
        pushInput (x);
        const float dry = allpassCoefficient * (delayed - dryOut) + dryIn;
        dryIn = delayed;
        dryOut = dry;
        const float processed = dry + mix.getNextValue() * (wet - dry);

        // Switching the fold on or off crossfades over 10 ms between the undelayed and delayed paths.
        return x + engage.getNextValue() * (processed - x);
    }

    /**
        sin(x) for any x: reduced to -pi..pi by whole turns, folded into -pi/2..pi/2, then a
        9th-order polynomial (error under 4e-6). Branch-free: audio-rate input makes branches
        here mispredict about half the time.
    */
    static float fastSin (float x) noexcept
    {
        constexpr float twoPi = juce::MathConstants<float>::twoPi, pi = juce::MathConstants<float>::pi;
        const float turns = x * (1.0f / twoPi);
        x -= twoPi * (float) (int) (turns + std::copysign (0.5f, turns));   // nearest whole turn: -pi..pi
        const float folded = std::copysign (std::min (std::abs (x), pi - std::abs (x)), x);   // sin (pi - x) = sin (x)
        const float x2 = folded * folded;
        return folded * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
    }

private:
    // The 31-tap half-band low-pass (Blackman-windowed sinc, cutoff a quarter of the 2x rate). Half its
    // taps are zero; the 16 left, scaled by 2 for upsampling and used as-is for decimation, are below.
    static constexpr size_t halfTaps = 16;
    static constexpr std::array<float, halfTaps> makeTaps (float scale)
    {
        std::array<float, halfTaps> taps {};
        double h[31] {}, sum = 0.0;
        for (int k = 0; k < 31; ++k)
        {
            const double offset = (double) k - 15.0;
            const double sinc = offset == 0.0 ? 1.0 : sinPi (0.5 * offset) / (juce::MathConstants<double>::pi * 0.5 * offset);
            const double window = 0.42 - 0.5 * cosTwoPi ((double) k / 30.0) + 0.08 * cosTwoPi (2.0 * (double) k / 30.0);
            h[k] = 0.5 * sinc * window;
            sum += h[k];
        }
        for (size_t i = 0; i < halfTaps; ++i)
            taps[i] = (float) (scale * h[2 * i] / sum);
        return taps;
    }
    // constexpr-friendly sin(pi x) and cos(2 pi x) for the tap table (Taylor series after range reduction).
    static constexpr double sinPi (double x)
    {
        while (x > 1.0)  x -= 2.0;
        while (x < -1.0) x += 2.0;
        const double r = juce::MathConstants<double>::pi * x;
        double term = r, total = r;
        for (int n = 1; n < 20; ++n) { term *= -r * r / (double) ((2 * n) * (2 * n + 1)); total += term; }
        return total;
    }
    static constexpr double cosTwoPi (double x)   { return sinPi (2.0 * x + 0.5); }

    static inline const std::array<float, halfTaps> upTaps   = makeTaps (2.0f);
    static inline const std::array<float, halfTaps> downTaps = makeTaps (1.0f);

    // Histories of 16 samples, stored newest-first and twice over (at k and k + 16), so the
    // newest 16 are always contiguous: the filter loops read straight through without
    // wrapping indices, which lets the compiler vectorise them.
    static constexpr size_t historySize = 16;
    using History = std::array<float, 2 * historySize>;

    static void push (History& h, size_t& pos, float x) noexcept
    {
        pos = (pos + historySize - 1) & (historySize - 1);   // moves backwards: newest first
        h[pos] = x;
        h[pos + historySize] = x;
    }

    static const float* recent (const History& h, size_t pos) noexcept   { return h.data() + pos; }

    /**
        15-tap dot product (the 16 taps minus the newest) as four interleaved partial sums. Without -ffast-math a compiler
        must add the 16 products strictly in order, one dependent addition after another;
        four independent chains run four times as fast. The order is fixed here, so the
        result is still the same on every machine and compiler.
    */
    static float dot15 (const float* taps, const float* x) noexcept
    {
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        for (size_t i = 0; i < 12; i += 4)
        {
            s0 += taps[i]     * x[i];
            s1 += taps[i + 1] * x[i + 1];
            s2 += taps[i + 2] * x[i + 2];
            s3 += taps[i + 3] * x[i + 3];
        }
        s0 += taps[12] * x[12];
        s1 += taps[13] * x[13];
        s2 += taps[14] * x[14];
        return (s0 + s1) + (s2 + s3);
    }

    void pushInput (float x) noexcept                  { push (input, inputPos, x); }
    void pushFolded (float even, float odd) noexcept
    {
        size_t evenPos = foldedPos;
        push (foldedEven, evenPos, even);
        push (foldedOdd, foldedPos, odd);
    }

    float removeDc (float x) noexcept
    {
        dcOut = x - dcIn + dcCoefficient * dcOut;
        dcIn = x;
        return dcOut;
    }

    /** xorshift32, mapped to -1..1. */
    float nextNoise() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) (juce::int32) state * (1.0f / 2147483648.0f);
    }

    SculptSettings pending;
    juce::SmoothedValue<float> noise { 0.0f }, drive { 3.0f }, mix { 0.0f }, engage { 0.0f };
    juce::uint32 state = 0x2545f491u;
    History input {}, foldedEven {}, foldedOdd {};
    size_t inputPos = 0, foldedPos = 0;
    float noiseGainsFor = -1.0f, waveGain = 1.0f, noiseGain = 0.0f;
    float dcIn = 0.0f, dcOut = 0.0f, dcCoefficient = 0.9987f;
    float dryIn = 0.0f, dryOut = 0.0f;
    static constexpr float allpassCoefficient = (1.0f - 0.5f) / (1.0f + 0.5f);   // Thiran: 0.5 samples of delay
};
