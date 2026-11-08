// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include "SampleData.h"
#include "Oscillator.h"

/**
    @file SampleLoader.h

    Turns files, embedded FLAC and the built-in pad into SampleData. Every
    function here allocates and may take tens of milliseconds for long
    files: call them from a loader thread or the message thread, never from
    the audio thread.
*/
namespace SampleLoader
{
    /** Samples whose FLAC encoding is larger than this are saved by path only. */
    constexpr size_t maxEmbeddedBytes = 10 * 1024 * 1024;

    /** Result of a load: the sample, or why there isn't one. */
    struct Result
    {
        std::unique_ptr<SampleData> sample;
        juce::String error;
    };

    /** Encodes level 0 of a sample as mono 24-bit FLAC. Returns an empty block on failure. */
    inline juce::MemoryBlock encodeFlac (const SampleData& sample)
    {
        juce::MemoryBlock block;
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (block, false);

        auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                        .withSampleRate (sample.getSampleRate())
                                                        .withNumChannels (1)
                                                        .withBitsPerSample (24));
        if (writer == nullptr)
            return {};

        // Write the 24-bit integers directly (left-justified in 32 bits, as AudioFormatWriter
        // expects): JUCE's float conversion scales by 2^31 - 1 and would move values above
        // 0.5 down by one step, breaking the exact round trip.
        const int length = sample.getLength (0);
        std::vector<int> ints ((size_t) length);
        for (int i = 0; i < length; ++i)
            ints[(size_t) i] = (int) ((juce::uint32) SampleData::toInt24 (sample.getData (0)[i]) << 8);

        const int* channels[] = { ints.data(), nullptr };
        const bool ok = writer->write (channels, length);
        writer.reset();   // flushes into `block`
        return ok ? block : juce::MemoryBlock();
    }

    /** Adds the embedded FLAC copy unless the sample is the default or too large to embed. */
    inline void attachEmbeddedFlac (SampleData& sample)
    {
        if (sample.getOrigin().isDefault)
            return;

        auto flac = encodeFlac (sample);
        if (flac.getSize() > 0 && flac.getSize() <= maxEmbeddedBytes)
            sample.setEmbeddedFlac (std::move (flac));
    }

    /** Decodes everything a reader offers (up to SampleData::maxSeconds). */
    inline Result fromReader (std::unique_ptr<juce::AudioFormatReader> reader, SampleData::Origin origin, bool normalise = true)
    {
        if (reader == nullptr)
            return { nullptr, "not an audio file NeonSynth can read (WAV, AIFF or FLAC)" };

        const auto maxSamples = (juce::int64) (SampleData::maxSeconds * reader->sampleRate) + 1;   // +1: lets create() notice truncation
        const auto numSamples = (int) juce::jmin (reader->lengthInSamples, maxSamples);
        const auto numChannels = (int) juce::jmax (1u, reader->numChannels);

        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        if (! reader->read (&buffer, 0, numSamples, 0, true, true))
            return { nullptr, "the file could not be read" };

        Result result;
        result.sample = SampleData::create (buffer, reader->sampleRate, std::move (origin), result.error, normalise);
        return result;
    }

    /** Loads a WAV, AIFF or FLAC file and prepares its embedded copy. */
    inline Result loadFile (const juce::File& file)
    {
        if (! file.existsAsFile())
            return { nullptr, "file not found" };

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        auto result = fromReader (std::move (reader), { file.getFileName(), file.getFullPathName(), false });

        if (result.sample != nullptr)
            attachEmbeddedFlac (*result.sample);
        return result;
    }

    /** Loads a sample from the FLAC copy embedded in a saved state. */
    inline Result loadEmbeddedFlac (const juce::MemoryBlock& flacData, SampleData::Origin origin)
    {
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::AudioFormatReader> reader (
            flac.createReaderFor (new juce::MemoryInputStream (flacData, true), true));

        auto result = fromReader (std::move (reader), std::move (origin), false);   // already prepared: keep it exactly
        if (result.sample != nullptr)
            result.sample->setEmbeddedFlac (flacData);   // already encoded: keep it as is
        return result;
    }

    //==========================================================================
    /**
        The built-in sample: a 4 s pad on C4, rendered from NeonSynth's own
        oscillators, so it carries no third-party licence and is identical on
        every machine. Three sawtooths detuned by -7, 0 and +7 cents plus a
        sine an octave down, through a gentle two-pole low-pass, with a 0.25 s
        attack and a 0.5 s fade at the end. The middle is steady, so it loops
        cleanly.
    */
    constexpr double defaultSampleRate    = 48000.0;
    constexpr double defaultSampleSeconds = 4.0;
    constexpr int    defaultRootNote      = 60;   // C4

    /**
        The oscillator the pad is rendered with: a frozen copy of PolyBlepOscillator as
        it was in v1.5.0 (sine and a 2-point PolyBLEP sawtooth). The pad must stay the
        same sound in every version, because sessions that use it don't store it (they
        save default="1"), so it can't follow improvements to the live oscillator.
    */
    struct PadOscillator
    {
        enum Waveform { sine = 0, sawtooth = 3 };

        void setWaveform (int w) noexcept                          { pendingWaveform = w; }
        void setFrequency (float hz, double sampleRate) noexcept   { phaseInc = (float) (hz / sampleRate); }
        int  getWaveform() const noexcept                          { return waveform; }

        float nextSample() noexcept
        {
            const float t = phase, dt = phaseInc;
            float value;
            if (waveform == sawtooth)
            {
                float blep = 0.0f;
                if (t < dt)             { const float u = t / dt;          blep = u + u - u * u - 1.0f; }
                else if (t > 1.0f - dt) { const float u = (t - 1.0f) / dt; blep = u * u + u + u + 1.0f; }
                value = 2.0f * t - 1.0f - blep;
            }
            else
            {
                value = std::sin (juce::MathConstants<float>::twoPi * t);
            }
            phase += dt;
            if (phase >= 1.0f)
            {
                phase -= 1.0f;
                waveform = pendingWaveform;
            }
            return value;
        }

        float phase = 0.0f, phaseInc = 0.0f;
        int   waveform = sine, pendingWaveform = sine;
    };

    inline std::unique_ptr<SampleData> makeDefaultSample()
    {
        const int length = (int) (defaultSampleSeconds * defaultSampleRate);
        const auto root = (float) juce::MidiMessage::getMidiNoteInHertz (defaultRootNote);

        std::array<PadOscillator, 4> oscillators;
        const float detuneCents[] = { -7.0f, 0.0f, 7.0f };
        for (size_t i = 0; i < 3; ++i)
        {
            oscillators[i].setWaveform (PadOscillator::sawtooth);
            oscillators[i].setFrequency (root * std::pow (2.0f, detuneCents[i] / 1200.0f), defaultSampleRate);
        }
        oscillators[3].setWaveform (PadOscillator::sine);
        oscillators[3].setFrequency (root * 0.5f, defaultSampleRate);

        // Waveform changes wait for a cycle boundary: run each oscillator into its shape,
        // staggered so the three saws don't start in phase.
        for (size_t i = 0; i < oscillators.size(); ++i)
            for (int s = 0, n = 200 + 61 * (int) i; s < n || oscillators[i].getWaveform() != (i < 3 ? PadOscillator::sawtooth : PadOscillator::sine); ++s)
                oscillators[i].nextSample();

        // Two-pole low-pass (state-variable, cutoff 1.8 kHz, Q 0.7).
        const float g = std::tan (juce::MathConstants<float>::pi * 1800.0f / (float) defaultSampleRate);
        const float k = 1.0f / 0.7f;
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float ic1 = 0.0f, ic2 = 0.0f;

        const int attack = (int) (0.25 * defaultSampleRate);
        const int fade   = (int) (0.5 * defaultSampleRate);

        juce::AudioBuffer<float> buffer (1, length);
        auto* out = buffer.getWritePointer (0);

        for (int i = 0; i < length; ++i)
        {
            const float saws = (oscillators[0].nextSample() + oscillators[1].nextSample() + oscillators[2].nextSample()) / 3.0f;
            const float x = 0.7f * saws + 0.3f * oscillators[3].nextSample();

            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;

            float env = 1.0f;
            if (i < attack)             env = (float) i / (float) attack;
            else if (i >= length - fade) env = (float) (length - i) / (float) fade;

            out[i] = v2 * env;
        }

        juce::String error;
        auto sample = SampleData::create (buffer, defaultSampleRate, { "Default pad", {}, true }, error);
        jassert (sample != nullptr);
        return sample;
    }
}
