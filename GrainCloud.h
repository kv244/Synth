// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include "SampleData.h"
#include "SampleReader.h"
#include <array>
#include <cstring>

// SIMD paths for the grain loop. x86-64 always has SSE2; AVX2 is compiled in too
// but only used when the CPU reports it (checked once, at run time), so one binary
// runs everywhere. Other CPUs (ARM) use the scalar loop.
#if defined (__SSE2__) || defined (_M_X64) || defined (_M_AMD64)
 #include <immintrin.h>
 #define NEONSYNTH_GRAINS_SSE2 1
 #define NEONSYNTH_GRAINS_AVX2 1
 #if defined (__GNUC__) || defined (__clang__)
  #define NEONSYNTH_TARGET_AVX2 __attribute__ ((target ("avx2")))   // GCC/Clang: allow AVX2 in this function only
 #else
  #define NEONSYNTH_TARGET_AVX2                                     // MSVC allows AVX2 intrinsics anywhere
 #endif
#else
 #define NEONSYNTH_GRAINS_SSE2 0
 #define NEONSYNTH_GRAINS_AVX2 0
#endif

#ifndef NEONSYNTH_GRAINS_AVX2_GATHER
 #define NEONSYNTH_GRAINS_AVX2_GATHER 1   // AVX2 reads with vgatherdps (1) or with scalar loads (0)
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
        manipulation rather than a conversion (see fractionOf). The window phase is age x windowStep,
        computed afresh for every sample rather than accumulated, so the
        scalar, SSE2 and AVX2 loops produce bit-identical results whatever
        order they work through the samples in (an accumulated phase would
        round differently in each).
    */
    struct Grain
    {
        const float* data = nullptr;   // the copy (level) it reads: grains outlive a retrigger to another pitch
        juce::uint64 position = 0, step = 0;
        int   age = 0;                 // samples rendered so far: the window phase is age x windowStep
        float windowStep = 0.0f;       // 1 / length
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

    //==========================================================================
    // The grain loop. Three versions of the same arithmetic, picked per call:
    //
    //   AVX2   8 samples per step, when the CPU has it (most x86 PCs since 2013)
    //   SSE2   4 samples per step (every x86-64 CPU)
    //   scalar 1 sample per step (the last few samples, and non-x86 CPUs)
    //
    // Each lane does exactly the scalar loop's operations in the same order, and
    // there is deliberately no FMA (a fused multiply-add rounds once instead of
    // twice), so all three render bit-identical audio: the same session sounds
    // the same on every machine, and the tests can compare the paths directly.
    //
    // What makes this loop expensive is not the arithmetic but the four reads
    // per sample from four different positions; the SIMD versions do the maths
    // for several samples at once and overlap those reads.

public:
    enum class SimdPath { automatic, scalar, sse2, avx2 };

    /** Forces one path (tests and benchmarks); automatic picks the best the CPU has. */
    static void forceSimdPath (SimdPath path) noexcept   { forcedPath().store (path); }

    /** The path render() uses now. */
    static SimdPath activeSimdPath() noexcept
    {
        const auto forced = forcedPath().load();
        if (forced != SimdPath::automatic)
            return forced == SimdPath::avx2 && ! cpuHasAVX2() ? SimdPath::sse2 : forced;
       #if NEONSYNTH_GRAINS_AVX2
        if (cpuHasAVX2()) return SimdPath::avx2;
       #endif
        return NEONSYNTH_GRAINS_SSE2 ? SimdPath::sse2 : SimdPath::scalar;
    }

    static bool cpuHasAVX2() noexcept
    {
        static const bool has = juce::SystemStats::hasAVX2();   // CPUID, read once
        return has;
    }

private:
    static std::atomic<SimdPath>& forcedPath() noexcept
    {
        static std::atomic<SimdPath> path { SimdPath::automatic };
        return path;
    }

    /** Catmull-Rom between x0 and x1, in Horner form. */
    static float cubic (float xm1, float x0, float x1, float x2, float t) noexcept
    {
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    /** The window, (4x(1-x))^2, at sample `age` of a grain. */
    static float windowAt (int age, float windowStep) noexcept
    {
        const float x = (float) age * windowStep;
        const float u = 4.0f * (x - x * x);
        return u * u;
    }

    /**
        Adds up to `count` samples of the grain to `out`; returns false once it
        has ended. The window is (4x(1-x))^2, a parabola-squared Hann shape
        (within 0.06 of sin^2(pi x), smooth, zero slope at both ends): four
        multiplies, no table.
    */
    bool renderGrain (Grain& g, float* __restrict out, int count) noexcept
    {
        const int n = juce::jmin (count, g.remaining);
        int s = 0;

        switch (activeSimdPath())
        {
           #if NEONSYNTH_GRAINS_AVX2
            case SimdPath::avx2:  s = renderGrainAVX2 (g, out, n); break;
           #endif
           #if NEONSYNTH_GRAINS_SSE2
            case SimdPath::sse2:  s = renderGrainSSE2 (g, out, n); break;
           #endif
            case SimdPath::automatic:
            case SimdPath::scalar:
            default:              break;
        }

        // Scalar loop: the samples the SIMD loop left over, or all of them.
        const float* __restrict const data = g.data;
        for (; s < n; ++s)
        {
            const float* d = data + (int) (g.position >> 32);
            out[s] += cubic (d[-1], d[0], d[1], d[2], fractionOf (g.position)) * windowAt (g.age, g.windowStep);
            g.position += g.step;
            ++g.age;
        }

        g.remaining -= n;
        return g.remaining > 0;
    }

   #if NEONSYNTH_GRAINS_SSE2
    /** 4 samples per step; returns how many samples it rendered (a multiple of 4). */
    static int renderGrainSSE2 (Grain& g, float* __restrict out, int n) noexcept
    {
        const float* __restrict const data = g.data;   // the output never overlaps the sample
        const juce::uint64 step = g.step;
        juce::uint64 position = g.position;

        const __m128 half = _mm_set1_ps (0.5f), oneHalf = _mm_set1_ps (1.5f), two = _mm_set1_ps (2.0f),
                     twoHalf = _mm_set1_ps (2.5f), four = _mm_set1_ps (4.0f), one = _mm_set1_ps (1.0f);
        const __m128 windowStep = _mm_set1_ps (g.windowStep);
        __m128i ages = _mm_setr_epi32 (g.age, g.age + 1, g.age + 2, g.age + 3);
        const __m128i ageStep = _mm_set1_epi32 (4);

        int s = 0;
        for (; s + 4 <= n; s += 4)
        {
            const juce::uint64 p0 = position, p1 = p0 + step, p2 = p1 + step, p3 = p2 + step;
            const float* d0 = data + (int) (p0 >> 32);
            const float* d1 = data + (int) (p1 >> 32);
            const float* d2 = data + (int) (p2 >> 32);
            const float* d3 = data + (int) (p3 >> 32);

            // SSE2 has no gather: the four reads per sample are scalar loads packed into lanes.
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

            const __m128 x = _mm_mul_ps (_mm_cvtepi32_ps (ages), windowStep);
            const __m128 u = _mm_mul_ps (four, _mm_sub_ps (x, _mm_mul_ps (x, x)));
            _mm_storeu_ps (out + s, _mm_add_ps (_mm_loadu_ps (out + s), _mm_mul_ps (v, _mm_mul_ps (u, u))));

            position = p3 + step;
            ages = _mm_add_epi32 (ages, ageStep);
        }

        g.position = position;
        g.age += s;
        return s;
    }
   #endif

   #if NEONSYNTH_GRAINS_AVX2
    /**
        8 samples per step; returns how many it rendered (a multiple of 8).
        The reads use the AVX2 gather instruction (vgatherdps): one instruction
        fetches the 8 lanes' values from 8 addresses, instead of 8 scalar
        loads and 7 inserts.
    */
    NEONSYNTH_TARGET_AVX2 static int renderGrainAVX2 (Grain& g, float* __restrict out, int n) noexcept
    {
        const float* __restrict const data = g.data;
        const juce::uint64 step = g.step;
        juce::uint64 position = g.position;

        const __m256 half = _mm256_set1_ps (0.5f), oneHalf = _mm256_set1_ps (1.5f), two = _mm256_set1_ps (2.0f),
                     twoHalf = _mm256_set1_ps (2.5f), four = _mm256_set1_ps (4.0f), one = _mm256_set1_ps (1.0f);
        const __m256 windowStep = _mm256_set1_ps (g.windowStep);
        __m256i ages = _mm256_add_epi32 (_mm256_set1_epi32 (g.age), _mm256_setr_epi32 (0, 1, 2, 3, 4, 5, 6, 7));
        const __m256i ageStep = _mm256_set1_epi32 (8);
        const __m256i exponent = _mm256_set1_epi32 (0x3f800000);

        int s = 0;
        for (; s + 8 <= n; s += 8)
        {
            // Sample indices and fraction bits of the 8 positions (32.32 fixed point), packed
            // straight into registers. (Writing them to a small array and loading that as a
            // vector stalls: the CPU can't forward eight 4-byte stores into one 32-byte load.)
            const juce::uint64 p0 = position, p1 = p0 + step, p2 = p1 + step, p3 = p2 + step,
                               p4 = p3 + step, p5 = p4 + step, p6 = p5 + step, p7 = p6 + step;
            const __m256i idx = _mm256_setr_epi32 ((int) (p0 >> 32), (int) (p1 >> 32), (int) (p2 >> 32), (int) (p3 >> 32),
                                                   (int) (p4 >> 32), (int) (p5 >> 32), (int) (p6 >> 32), (int) (p7 >> 32));
            const __m256i frac = _mm256_setr_epi32 ((int) ((juce::uint32) p0 >> 9), (int) ((juce::uint32) p1 >> 9),
                                                    (int) ((juce::uint32) p2 >> 9), (int) ((juce::uint32) p3 >> 9),
                                                    (int) ((juce::uint32) p4 >> 9), (int) ((juce::uint32) p5 >> 9),
                                                    (int) ((juce::uint32) p6 >> 9), (int) ((juce::uint32) p7 >> 9));
           #if NEONSYNTH_GRAINS_AVX2_GATHER
            const __m256 xm1 = _mm256_i32gather_ps (data - 1, idx, 4);
            const __m256 x0  = _mm256_i32gather_ps (data,     idx, 4);
            const __m256 x1  = _mm256_i32gather_ps (data + 1, idx, 4);
            const __m256 x2  = _mm256_i32gather_ps (data + 2, idx, 4);
           #else
            juce::ignoreUnused (idx);
            const float* d0 = data + (int) (p0 >> 32); const float* d1 = data + (int) (p1 >> 32);
            const float* d2 = data + (int) (p2 >> 32); const float* d3 = data + (int) (p3 >> 32);
            const float* d4 = data + (int) (p4 >> 32); const float* d5 = data + (int) (p5 >> 32);
            const float* d6 = data + (int) (p6 >> 32); const float* d7 = data + (int) (p7 >> 32);
            const __m256 xm1 = _mm256_setr_ps (d0[-1], d1[-1], d2[-1], d3[-1], d4[-1], d5[-1], d6[-1], d7[-1]);
            const __m256 x0  = _mm256_setr_ps (d0[0],  d1[0],  d2[0],  d3[0],  d4[0],  d5[0],  d6[0],  d7[0]);
            const __m256 x1  = _mm256_setr_ps (d0[1],  d1[1],  d2[1],  d3[1],  d4[1],  d5[1],  d6[1],  d7[1]);
            const __m256 x2  = _mm256_setr_ps (d0[2],  d1[2],  d2[2],  d3[2],  d4[2],  d5[2],  d6[2],  d7[2]);
           #endif
            const juce::uint64 p = p7 + step;

            const __m256i bits = _mm256_or_si256 (exponent, frac);
            const __m256 t = _mm256_sub_ps (_mm256_castsi256_ps (bits), one);

            const __m256 c1 = _mm256_mul_ps (half, _mm256_sub_ps (x1, xm1));
            const __m256 c2 = _mm256_sub_ps (_mm256_add_ps (_mm256_sub_ps (xm1, _mm256_mul_ps (twoHalf, x0)), _mm256_mul_ps (two, x1)), _mm256_mul_ps (half, x2));
            const __m256 c3 = _mm256_add_ps (_mm256_mul_ps (half, _mm256_sub_ps (x2, xm1)), _mm256_mul_ps (oneHalf, _mm256_sub_ps (x0, x1)));
            const __m256 v  = _mm256_add_ps (_mm256_mul_ps (_mm256_add_ps (_mm256_mul_ps (_mm256_add_ps (_mm256_mul_ps (c3, t), c2), t), c1), t), x0);

            const __m256 x = _mm256_mul_ps (_mm256_cvtepi32_ps (ages), windowStep);
            const __m256 u = _mm256_mul_ps (four, _mm256_sub_ps (x, _mm256_mul_ps (x, x)));
            _mm256_storeu_ps (out + s, _mm256_add_ps (_mm256_loadu_ps (out + s), _mm256_mul_ps (v, _mm256_mul_ps (u, u))));

            position = p;
            ages = _mm256_add_epi32 (ages, ageStep);
        }

        g.position = position;
        g.age += s;

        // Clear the upper halves of the 256-bit registers before returning to code
        // compiled for SSE: on some Intel CPUs mixing the two without this costs a
        // slow state transition on every SSE instruction that follows.
        _mm256_zeroupper();
        return s;
    }
   #endif

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
        g.age         = 0;
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
