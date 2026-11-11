// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

/**
    @file SampleData.h

    The audio that the Sample and Granular sources play: one mono buffer,
    prepared once on a loader thread and never changed afterwards, so the
    audio thread can read it without locks.

    Preparation (SampleData::create):
      - stereo or multichannel input is averaged to mono;
      - anything past maxSeconds is cut off;
      - the peak is normalised to -1 dBFS and every sample is rounded to the
        24-bit grid (k / 2^23), which a float holds exactly, so saving it as
        24-bit FLAC and loading it back gives the identical buffer (and the
        identical content hash);
      - two band-limited copies at 1/2 and 1/4 of the sample rate are built
        (half-band low-pass, then every second sample). A voice pitching far
        up reads a smaller copy, so it never skips more than about one source
        sample per output sample and the decimation filter has already
        removed what would alias.

    Every level carries `padding` zero samples before and after its data, so
    an interpolator can read a few samples either side of any position
    without bounds checks.
*/
class SampleData
{
public:
    static constexpr double maxSeconds = 60.0;     ///< longer files are cut off
    static constexpr int    numLevels  = 3;        ///< full, 1/2 and 1/4 rate
    static constexpr int    padding    = 4;        ///< zero samples before and after each level
    static constexpr float  peakGain   = 0.89125094f;   // -1 dBFS

    /** Where the audio came from, for the editor and the saved state. */
    struct Origin
    {
        juce::String name;     ///< what the editor shows: the file name, or "Default pad"
        juce::String path;     ///< full path of the file it was loaded from; empty for the default sample
        bool isDefault = false;
    };

    /**
        Builds the sample from decoded audio. Returns nullptr (and sets `error`)
        for empty or silent input. Allocates: loader or message thread only.

        `normalise` is false only for audio that is already a prepared sample
        (the FLAC copy embedded in a saved state): it is then kept exactly.
    */
    static std::unique_ptr<SampleData> create (const juce::AudioBuffer<float>& source, double sourceSampleRate,
                                               Origin origin, juce::String& error, bool normalise = true)
    {
        if (source.getNumSamples() <= 0 || source.getNumChannels() <= 0 || sourceSampleRate <= 0.0)
        {
            error = "the file has no audio";
            return nullptr;
        }

        const int length = (int) juce::jmin ((juce::int64) source.getNumSamples(),
                                             (juce::int64) (maxSeconds * sourceSampleRate));

        // The whole-array steps (mono mix, peak, normalise) use JUCE's FloatVectorOperations,
        // which run them with SSE / NEON. Same arithmetic per element as the loops they
        // replace, so the prepared sample (and its hash) is unchanged.
        std::vector<float> mono ((size_t) (length + 2 * padding), 0.0f);
        float* const samples = mono.data() + padding;
        const float channelScale = 1.0f / (float) source.getNumChannels();
        for (int ch = 0; ch < source.getNumChannels(); ++ch)
            juce::FloatVectorOperations::addWithMultiply (samples, source.getReadPointer (ch), channelScale, length);

        const auto extremes = juce::FloatVectorOperations::findMinAndMax (samples, length);
        const float peak = juce::jmax (-extremes.getStart(), extremes.getEnd(), 0.0f);

        if (peak < 1.0e-6f)
        {
            error = "the file is silent";
            return nullptr;
        }

        // Normalise to -1 dBFS, then round to the 24-bit grid (a scalar loop: rounding to
        // the grid has no FloatVectorOperations equivalent, and this runs once per load).
        if (normalise)
            juce::FloatVectorOperations::multiply (samples, peakGain / peak, length);
        for (int i = 0; i < length; ++i)
            samples[i] = quantise24 (samples[i]);

        std::unique_ptr<SampleData> data (new SampleData());
        data->origin     = std::move (origin);
        data->sampleRate = sourceSampleRate;
        data->truncated  = length < source.getNumSamples();
        data->levels[0]  = std::move (mono);
        data->lengths[0] = length;

        for (int level = 1; level < numLevels; ++level)
        {
            data->levels[(size_t) level]  = decimate (data->levels[(size_t) (level - 1)], data->lengths[(size_t) (level - 1)]);
            data->lengths[(size_t) level] = data->lengths[(size_t) (level - 1)] / 2;
        }

        data->hash = hashOf (data->levels[0].data() + padding, length);
        return data;
    }

    /** Samples in a level (excluding padding). Level 0 is the full-rate buffer. */
    int getLength (int level = 0) const noexcept          { return lengths[(size_t) level]; }

    /** First real sample of a level; `padding` readable zeros sit either side. */
    const float* getData (int level = 0) const noexcept   { return levels[(size_t) level].data() + padding; }

    /** Sample rate of level 0 (level n runs at this / 2^n). */
    double getSampleRate() const noexcept                 { return sampleRate; }

    double getLengthSeconds() const noexcept              { return (double) lengths[0] / sampleRate; }

    /** True if the source was longer than maxSeconds and was cut off. */
    bool wasTruncated() const noexcept                    { return truncated; }

    const Origin& getOrigin() const noexcept              { return origin; }

    /** 64-bit FNV-1a of the 24-bit sample values, as 16 hex digits. Identical buffers give identical hashes. */
    const juce::String& getHash() const noexcept          { return hash; }

    /**
        The sample as 24-bit FLAC, for embedding in the saved state. Empty for
        the default sample and for samples above the embedding limit. Set once
        by the loader before the sample is published.
    */
    const juce::MemoryBlock& getEmbeddedFlac() const noexcept   { return embeddedFlac; }
    void setEmbeddedFlac (juce::MemoryBlock flac)               { embeddedFlac = std::move (flac); }

    /**
        Number of voices playing this sample. Voices increment it when a note
        starts and decrement it when the note has fully faded (audio thread);
        SampleSlot only frees a retired sample once this is back to zero.
    */
    mutable std::atomic<int> users { 0 };

    /**
        A separate sample with the same audio, levels, hash and origin, and no users. Copying
        a prepared sample is far cheaper than preparing one (SampleLoader keeps one prepared
        default pad and hands out copies).
        Allocates: loader or message thread only.
    */
    std::unique_ptr<SampleData> copy() const
    {
        std::unique_ptr<SampleData> data (new SampleData());
        data->origin       = origin;
        data->sampleRate   = sampleRate;
        data->truncated    = truncated;
        data->levels       = levels;
        data->lengths      = lengths;
        data->hash         = hash;
        data->embeddedFlac = embeddedFlac;
        return data;
    }

    static constexpr float fullScale24 = 8388608.0f;   // 2^23

    /** The 24-bit integer a sample on the grid stands for (-2^23 .. 2^23 - 1). */
    static juce::int32 toInt24 (float x) noexcept
    {
        return (juce::int32) juce::jlimit (-8388608.0f, 8388607.0f, std::round (x * fullScale24));
    }

    /** Rounds to the nearest 24-bit sample value, k / 2^23; a float holds every such value exactly. */
    static float quantise24 (float x) noexcept
    {
        return (float) toInt24 (x) / fullScale24;
    }

private:
    SampleData() = default;

    /** Half-band-ish low-pass (cutoff 0.21 of the input rate) and keep every second sample. */
    static std::vector<float> decimate (const std::vector<float>& paddedInput, int inputLength)
    {
        static const auto taps = makeLowPass();
        constexpr int half = (int) decimationTaps / 2;

        const int outputLength = inputLength / 2;
        std::vector<float> out ((size_t) (outputLength + 2 * padding), 0.0f);
        const float* in = paddedInput.data() + padding;

        for (int n = 0; n < outputLength; ++n)
        {
            const int centre = 2 * n;
            double acc = 0.0;
            for (int k = 0; k < (int) decimationTaps; ++k)
            {
                const int i = centre + k - half;
                if (i >= 0 && i < inputLength)
                    acc += (double) taps[(size_t) k] * in[i];
            }
            out[(size_t) (padding + n)] = (float) acc;
        }
        return out;
    }

    static constexpr size_t decimationTaps = 95;

    /** Blackman-windowed sinc, cutoff 0.21 x the input rate, unity gain at DC. */
    static std::array<float, decimationTaps> makeLowPass()
    {
        std::array<double, decimationTaps> h {};
        constexpr double cutoff = 0.21;
        const double centre = (double) (decimationTaps - 1) / 2.0;
        double sum = 0.0;

        for (size_t k = 0; k < decimationTaps; ++k)
        {
            const double x = (double) k - centre;
            const double sinc = x == 0.0 ? 2.0 * cutoff
                                         : std::sin (juce::MathConstants<double>::twoPi * cutoff * x) / (juce::MathConstants<double>::pi * x);
            const double phase = juce::MathConstants<double>::twoPi * (double) k / (double) (decimationTaps - 1);
            const double window = 0.42 - 0.5 * std::cos (phase) + 0.08 * std::cos (2.0 * phase);
            h[k] = sinc * window;
            sum += h[k];
        }

        std::array<float, decimationTaps> taps {};
        for (size_t k = 0; k < decimationTaps; ++k)
            taps[k] = (float) (h[k] / sum);
        return taps;
    }

    static juce::String hashOf (const float* samples, int length)
    {
        juce::uint64 h = 14695981039346656037ull;
        for (int i = 0; i < length; ++i)
        {
            const auto v = (juce::uint32) toInt24 (samples[i]);
            for (int b = 0; b < 3; ++b)
            {
                h ^= (v >> (8 * b)) & 0xffu;
                h *= 1099511628211ull;
            }
        }
        return juce::String::toHexString ((juce::int64) h).paddedLeft ('0', 16);
    }

    Origin origin;
    double sampleRate = 48000.0;
    bool   truncated = false;
    std::array<std::vector<float>, numLevels> levels;
    std::array<int, numLevels> lengths {};
    juce::String hash;
    juce::MemoryBlock embeddedFlac;
};
