// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include "SampleData.h"
#include "SampleReader.h"
#include <array>
#include <cstring>

#if defined (__SSE2__) || defined (_M_X64) || defined (_M_AMD64)
 #include <emmintrin.h>
 #define NEONSYNTH_GRAINS_SSE2 1
#else
 #define NEONSYNTH_GRAINS_SSE2 0
#endif

/**
    @file GrainCloud.h

    The granular source for one voice: short windowed grains read from
    the sample near a moving position, summed into a cloud.

    Scheduling. A countdown starts a grain every 1 / Density seconds,
    jittered by +-30% so dense clouds don't buzz at the density rate.

    Each grain, when it starts, takes
      - a start point: the voice's scan head +- Spray x half the file
        (Spray 1 reaches across the whole file), kept inside the sample;
      - a length: Size (5 - 500 ms of output);
      - a pitch: the note's ratio to the root, as in SampleReader, +- Pitch
        jitter (up to 100 cents);
      - a window: (4x(1-x))^2, a Hann-shaped bell (within 0.06 of sin^2),
        smooth with zero slope at both ends, computed in four multiplies.
    Later knob changes affect the next grains, never one that is playing, so
    turning a knob can't click.

    The scan head starts at Position and moves at Scan x real time (-2x to
    +2x of the file's own speed; 0 freezes it), wrapping at the file's ends.
    Position itself stays live: moving it moves the whole cloud.

    Level. Grains overlap by about Density x Size; the sum is scaled by
    1 / sqrt(max(1, overlap)) so thin and dense clouds sound about equally
    loud. The output soft clipper still catches peaks.

    Voices. Up to 32 grains at once; an onset is skipped while all 32 are
    busy (stealing one would click). Notes pitched well above the root read
    the sample's band-limited copies, as in SampleReader.

    Determinism. Randomness comes from a xorshift generator seeded per note
    (from the note, the voice and a counter the processor resets with the
    voices), so the same MIDI renders the same audio.

    Holds a reference on the sample from begin() to end(). Audio thread only:
    no allocation, no locks.
*/
class GrainCloud
{
public:
    static constexpr int maxGrains = 32;

    /** Per-block settings from the Grain parameters. */
    struct Settings
    {
        float position = 0.5f;       ///< 0..1 of the sample
        float scan = 0.0f;           ///< -2..2 x the file's own speed
        float sizeSeconds = 0.08f;   ///< 0.005..0.5
        float density = 30.0f;       ///< grains per second, 1..200
        float spray = 0.1f;          ///< 0..1
        float jitterCents = 0.0f;    ///< 0..100
    };

    GrainCloud() = default;

    GrainCloud (const GrainCloud&) = delete;
    GrainCloud& operator= (const GrainCloud&) = delete;
    ~GrainCloud() = default;   // never touches the sample: at shutdown it may already be gone

    /**
        Starts a note. Grains already sounding from the previous note on this
        voice play out (they are windowed, so a retrigger never clicks), unless
        the sample itself has changed. Returns false if there is no sample.
    */
    bool begin (const SampleData* newSample, int midiNote, int rootNote, double hostSampleRate,
                const Settings& newSettings, juce::uint32 seed) noexcept
    {
        if (newSample == nullptr || newSample->getLength() <= 0 || hostSampleRate <= 0.0)
        {
            end();
            return false;
        }

        if (newSample != sample)
        {
            end();
            sample = newSample;
            sample->users.fetch_add (1);
        }

        hostRate = hostSampleRate;
        rng = seed != 0 ? seed : 0x9e3779b9u;

        const double ratio = std::pow (2.0, (midiNote - rootNote) / 12.0) * sample->getSampleRate() / hostRate;
        level = 0;
        while (level < SampleData::numLevels - 1 && ratio / (double) (1 << level) > SampleReader::maxFullRateIncrement)
            ++level;
        noteIncrement = ratio / (double) (1 << level);

        settings = newSettings;
        scanOffset = 0.0;
        samplesToNextGrain = 0;   // the first grain starts at once
        spawning = true;
        return true;
    }

    /** Starts no more grains; those sounding play out (the voice moved to another source). */
    void stopSpawning() noexcept   { spawning = false; }

    /** False once it has stopped spawning and its last grain has ended (it can then be end()ed). */
    bool isSounding() const noexcept   { return sample != nullptr && (spawning || numActive > 0); }

    /** Settings for the grains that start from now on (Position and Scan act immediately). */
    void setSettings (const Settings& newSettings) noexcept   { settings = newSettings; }

    /** Ends the note at once: drops every grain and releases the sample. Safe when idle. */
    void end() noexcept
    {
        numActive = 0;
        spawning = false;
        if (sample != nullptr)
            sample->users.fetch_sub (1);
        sample = nullptr;
    }

    bool isActive() const noexcept           { return sample != nullptr; }
    int getNumActiveGrains() const noexcept  { return numActive; }
    juce::int64 getGrainsStarted() const noexcept   { return grainsStarted; }
    int getLevel() const noexcept            { return level; }

    /** Read position (0..1) of up to `max` sounding grains, for display; returns how many. */
    int getGrainPositions (float* dest, int max) const noexcept
    {
        const int n = juce::jmin (max, numActive);
        for (int i = 0; i < n; ++i)
            dest[i] = (float) ((double) grains[(size_t) i].position * (1.0 / 4294967296.0)) * grains[(size_t) i].lengthScale;
        return n;
    }

    /**
        Renders `numSamples` of the cloud into `out` (overwriting it). Grains
        start on their exact sample: the block is split at each onset, and
        between onsets every grain renders its stretch in one tight loop.
    */
    void render (float* out, int numSamples) noexcept
    {
        std::fill (out, out + numSamples, 0.0f);
        if (sample == nullptr)
            return;

        int pos = 0;
        while (pos < numSamples)
        {
            if (spawning && samplesToNextGrain <= 0)
            {
                startGrain();
                const double interval = hostRate / (double) juce::jlimit (1.0f, 200.0f, settings.density);
                samplesToNextGrain = juce::jmax (1, (int) (interval * (0.7 + 0.6 * random01())));
            }

            const int segment = spawning ? juce::jmin (numSamples - pos, samplesToNextGrain) : numSamples - pos;

            for (int i = 0; i < numActive;)
            {
                if (renderGrain (grains[(size_t) i], out + pos, segment))
                    ++i;
                else
                    grains[(size_t) i] = grains[(size_t) --numActive];   // finished: swap in the last one
            }

            // The scan head moves through the file at Scan x its own speed.
            scanOffset += (double) settings.scan * (double) segment / (sample->getLengthSeconds() * hostRate);
            scanOffset -= std::floor (scanOffset);

            samplesToNextGrain -= segment;
            pos += segment;
        }

        const float overlap = juce::jlimit (0.005f, 0.5f, settings.sizeSeconds) * juce::jlimit (1.0f, 200.0f, settings.density);
        const float gain = 1.0f / std::sqrt (juce::jmax (1.0f, overlap));
        for (int i = 0; i < numSamples; ++i)
            out[i] *= gain;
    }

    /** One sample (for tests; the voice renders whole blocks). */
    float nextSample() noexcept
    {
        float x;
        render (&x, 1);
        return x;
    }

private:
    /**
        A grain's read position is 32.32 fixed point (sample index in the top
        32 bits): stepping it is one integer add, so nothing slow sits on the
        loop-carried path, and its fraction becomes a float by bit
        manipulation rather than a conversion (see fractionOf). The window phase is a float stepped
        0 -> 1 over the grain (its end is set by `remaining`, so rounding
        drift can't lengthen it).
    */
    struct Grain
    {
        const float* data = nullptr;   // the copy (level) it reads: grains outlive a retrigger to another pitch
        juce::uint64 position = 0, step = 0;
        float windowPhase = 0.0f, windowStep = 0.0f;   // 0..1 through the grain
        int   remaining = 0;           // output samples left
        float lengthScale = 1.0f;      // 1 / that copy's length, for display
    };

    /**
        The fractional part of a 32.32 position as a float in [0, 1), built
        from its bits (top 23 fraction bits as the mantissa of a float in
        [1, 2), minus 1). Avoids an int-to-float conversion, which on x64
        only partly writes its register and so chains the loop's iterations.
    */
    static float fractionOf (juce::uint64 position) noexcept
    {
        const juce::uint32 bits = 0x3f800000u | ((juce::uint32) position >> 9);
        float f;
        std::memcpy (&f, &bits, sizeof (f));
        return f - 1.0f;
    }

    /** Catmull-Rom between x0 and x1, in Horner form. */
    static float cubic (float xm1, float x0, float x1, float x2, float t) noexcept
    {
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    /**
        Adds up to `count` samples of the grain to `out`; returns false once it
        has ended. The window is (4x(1-x))^2, a parabola-squared Hann shape
        (within 0.06 of sin^2(pi x), smooth, zero slope at both ends): four
        multiplies, no table. On x86 four output samples are computed at once
        with SSE2 (the four reads are still scalar loads); the remainder, and
        other CPUs, use the scalar loop. Both compute the same formula.
    */
    bool renderGrain (Grain& g, float* __restrict out, int count) noexcept
    {
        const int n = juce::jmin (count, g.remaining);
        const float* __restrict const data = g.data;   // the output never overlaps the sample
        const juce::uint64 step = g.step;
        const float dx = g.windowStep;
        juce::uint64 position = g.position;
        float x = g.windowPhase;
        int s = 0;

       #if NEONSYNTH_GRAINS_SSE2
        {
            const __m128 half = _mm_set1_ps (0.5f), oneHalf = _mm_set1_ps (1.5f), two = _mm_set1_ps (2.0f),
                         twoHalf = _mm_set1_ps (2.5f), four = _mm_set1_ps (4.0f), one = _mm_set1_ps (1.0f);
            const __m128 dx4 = _mm_set1_ps (4.0f * dx);
            __m128 xs = _mm_setr_ps (x, x + dx, x + 2.0f * dx, x + 3.0f * dx);

            for (; s + 4 <= n; s += 4)
            {
                const juce::uint64 p0 = position, p1 = p0 + step, p2 = p1 + step, p3 = p2 + step;
                const float* d0 = data + (int) (p0 >> 32);
                const float* d1 = data + (int) (p1 >> 32);
                const float* d2 = data + (int) (p2 >> 32);
                const float* d3 = data + (int) (p3 >> 32);

                const __m128 xm1 = _mm_setr_ps (d0[-1], d1[-1], d2[-1], d3[-1]);
                const __m128 x0  = _mm_setr_ps (d0[0],  d1[0],  d2[0],  d3[0]);
                const __m128 x1  = _mm_setr_ps (d0[1],  d1[1],  d2[1],  d3[1]);
                const __m128 x2  = _mm_setr_ps (d0[2],  d1[2],  d2[2],  d3[2]);

                // Fractions from the bits (see fractionOf), four at once.
                const __m128i bits = _mm_or_si128 (_mm_set1_epi32 (0x3f800000),
                                                   _mm_setr_epi32 ((int) ((juce::uint32) p0 >> 9), (int) ((juce::uint32) p1 >> 9),
                                                                   (int) ((juce::uint32) p2 >> 9), (int) ((juce::uint32) p3 >> 9)));
                const __m128 t = _mm_sub_ps (_mm_castsi128_ps (bits), one);

                const __m128 c1 = _mm_mul_ps (half, _mm_sub_ps (x1, xm1));
                const __m128 c2 = _mm_sub_ps (_mm_add_ps (_mm_sub_ps (xm1, _mm_mul_ps (twoHalf, x0)), _mm_mul_ps (two, x1)), _mm_mul_ps (half, x2));
                const __m128 c3 = _mm_add_ps (_mm_mul_ps (half, _mm_sub_ps (x2, xm1)), _mm_mul_ps (oneHalf, _mm_sub_ps (x0, x1)));
                const __m128 v  = _mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (_mm_add_ps (_mm_mul_ps (c3, t), c2), t), c1), t), x0);

                const __m128 u = _mm_mul_ps (four, _mm_sub_ps (xs, _mm_mul_ps (xs, xs)));
                _mm_storeu_ps (out + s, _mm_add_ps (_mm_loadu_ps (out + s), _mm_mul_ps (v, _mm_mul_ps (u, u))));

                position = p3 + step;
                xs = _mm_add_ps (xs, dx4);
            }
            x = _mm_cvtss_f32 (xs);
        }
       #endif

        for (; s < n; ++s)
        {
            const float* d = data + (int) (position >> 32);
            const float u = 4.0f * (x - x * x);
            out[s] += cubic (d[-1], d[0], d[1], d[2], fractionOf (position)) * (u * u);
            position += step;
            x += dx;
        }

        g.position = position;
        g.windowPhase = x;
        g.remaining -= n;
        return g.remaining > 0;
    }

    void startGrain() noexcept
    {
        if (numActive >= maxGrains)
            return;   // all busy: skip this onset rather than cut a grain off

        const int length = sample->getLength (level);
        const int sizeSamples = juce::jmax (2, (int) (juce::jlimit (0.005, 0.5, (double) settings.sizeSeconds) * hostRate));

        double increment = noteIncrement * std::pow (2.0, (double) (settings.jitterCents * (2.0f * random01() - 1.0f)) / 1200.0);
        const double span = (double) sizeSamples * increment;   // source samples the grain will read
        if (span >= (double) length - 3.0)
            increment = juce::jmax (0.0, ((double) length - 3.0) / (double) sizeSamples);   // longer than the file: slow it to fit

        // Centre of the cloud, then the spray around it; the grain must fit inside the file.
        double centre = (double) juce::jlimit (0.0f, 1.0f, settings.position) + scanOffset;
        centre -= std::floor (centre);
        const double spread = 0.5 * (double) juce::jlimit (0.0f, 1.0f, settings.spray) * (2.0 * random01() - 1.0);
        const double lastStart = juce::jmax (0.0, (double) length - (double) sizeSamples * increment - 3.0);
        const double start = juce::jlimit (0.0, lastStart, (centre + spread) * (double) length);

        Grain g;
        g.data        = sample->getData (level);
        g.position    = (juce::uint64) (start * 4294967296.0);
        g.step        = (juce::uint64) (increment * 4294967296.0);
        g.windowPhase = 0.0f;
        g.windowStep  = 1.0f / (float) sizeSamples;
        g.remaining   = sizeSamples;
        g.lengthScale = 1.0f / (float) juce::jmax (1, length);

        grains[(size_t) numActive++] = g;
        ++grainsStarted;
    }

    /** xorshift32 in [0, 1). */
    float random01() noexcept
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (float) (rng >> 8) * (1.0f / 16777216.0f);
    }

    std::array<Grain, maxGrains> grains {};
    int numActive = 0;

    const SampleData* sample = nullptr;
    Settings settings;
    double hostRate = 48000.0, noteIncrement = 1.0, scanOffset = 0.0;
    int level = 0, samplesToNextGrain = 0;
    bool spawning = false;
    juce::uint32 rng = 1;
    juce::int64 grainsStarted = 0;
};
