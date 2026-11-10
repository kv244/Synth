// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for SampleData (how a sample is prepared), SampleLoader (files,
    embedded FLAC, the built-in pad) and SampleSlot (the lock-free hand-off and
    when replaced samples are freed).

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target SampleDataTests
    Run the resulting SampleDataTests executable; it exits non-zero on failure.
*/
#include <juce_audio_formats/juce_audio_formats.h>
#include "../SampleData.h"
#include "../SampleSlot.h"
#include "../SampleLoader.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <thread>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> sine (double hz, double seconds, float amplitude, int channels = 1, double sampleRate = rate)
    {
        juce::AudioBuffer<float> b (channels, (int) (seconds * sampleRate));
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sampleRate));
        return b;
    }

    std::unique_ptr<SampleData> make (const juce::AudioBuffer<float>& b, double sampleRate = rate)
    {
        juce::String error;
        return SampleData::create (b, sampleRate, { "test", {}, false }, error);
    }

    double rms (const float* x, int n, int skip = 200)
    {
        double sum = 0.0;
        for (int i = skip; i < n - skip; ++i)
            sum += (double) x[i] * x[i];
        return std::sqrt (sum / (n - 2 * skip));
    }

    int risingZeroCrossings (const float* x, int n)
    {
        int count = 0;
        for (int i = 1; i < n; ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
                ++count;
        return count;
    }

    /** Energy of x at frequency hz (Goertzel). */
    double energyAt (const float* x, int n, double hz, double sampleRate)
    {
        const double w = juce::MathConstants<double>::twoPi * hz / sampleRate, c = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double s0 = x[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    }
}

int main()
{
    std::printf ("Preparation\n");
    {
        auto b = sine (1000.0, 1.0, 0.5f, 2);
        b.applyGain (1, 0, b.getNumSamples(), 0.5f);   // right channel at half level: mono = 0.375 x sine
        auto s = make (b);
        check (s != nullptr && s->getLength() == 48000, "a stereo file becomes one mono buffer of the same length");

        float peak = 0.0f;
        bool onGrid = true;
        for (int i = 0; i < s->getLength(); ++i)
        {
            const float v = s->getData()[i];
            peak = std::max (peak, std::abs (v));
            const double scaled = (double) v * 8388608.0;
            onGrid = onGrid && std::abs (scaled - std::round (scaled)) < 1.0e-3;
        }
        check (std::abs (juce::Decibels::gainToDecibels (peak) + 1.0f) < 0.01f, "the peak is normalised to -1 dBFS");
        check (onGrid, "every sample sits on the 24-bit grid (so a 24-bit FLAC round trip is exact)");
        check (s->getData()[-1] == 0.0f && s->getData()[-SampleData::padding] == 0.0f
               && s->getData()[s->getLength() + SampleData::padding - 1] == 0.0f, "zero padding before and after the data");

        juce::String error;
        juce::AudioBuffer<float> silent (1, 1000);
        silent.clear();
        check (SampleData::create (silent, rate, {}, error) == nullptr && error.isNotEmpty(), "a silent file is refused with a reason");
        juce::AudioBuffer<float> empty;
        check (SampleData::create (empty, rate, {}, error) == nullptr, "an empty file is refused");

        auto longFile = sine (100.0, 61.0, 0.5f, 1, 8000.0);
        auto cut = make (longFile, 8000.0);
        check (cut != nullptr && cut->getLength() == 480000 && cut->wasTruncated(), "a 61 s file is cut to 60 s and flagged");
        check (! s->wasTruncated(), "a short file is not flagged as cut");
    }

    std::printf ("Decimated copies\n");
    {
        auto s = make (sine (1000.0, 1.0, 0.5f));
        check (s->getLength (1) == 24000 && s->getLength (2) == 12000, "levels 1 and 2 are 1/2 and 1/4 as long");
        check (std::abs (risingZeroCrossings (s->getData (1), s->getLength (1)) - 1000) <= 1, "level 1 keeps the pitch (1 kHz at 24 kHz)");
        check (std::abs (risingZeroCrossings (s->getData (2), s->getLength (2)) - 1000) <= 1, "level 2 keeps the pitch (1 kHz at 12 kHz)");

        const auto passBand = make (sine (4000.0, 1.0, 0.5f));
        const double in4k = rms (passBand->getData (0), passBand->getLength (0)), out4k = rms (passBand->getData (1), passBand->getLength (1));
        check (std::abs (juce::Decibels::gainToDecibels (out4k / in4k)) < 0.1, "4 kHz passes into level 1 within 0.1 dB");

        const auto alias1 = make (sine (15000.0, 1.0, 0.5f));
        const double r1 = juce::Decibels::gainToDecibels (rms (alias1->getData (1), alias1->getLength (1)) / rms (alias1->getData (0), alias1->getLength (0)));
        char what[128];
        std::snprintf (what, sizeof (what), "15 kHz (above level 1's Nyquist) is removed from level 1: %.1f dB", r1);
        check (r1 < -60.0, what);

        const auto alias2 = make (sine (8000.0, 1.0, 0.5f));
        const double r2 = juce::Decibels::gainToDecibels (rms (alias2->getData (2), alias2->getLength (2)) / rms (alias2->getData (0), alias2->getLength (0)));
        std::snprintf (what, sizeof (what), "8 kHz (above level 2's Nyquist) is removed from level 2: %.1f dB", r2);
        check (r2 < -60.0, what);
    }

    std::printf ("Hash and FLAC\n");
    {
        auto a = make (sine (440.0, 0.5, 0.3f));
        auto b = make (sine (440.0, 0.5, 0.3f));
        auto c = make (sine (441.0, 0.5, 0.3f));
        check (a->getHash().length() == 16 && a->getHash() == b->getHash(), "the same audio gives the same 16-digit hash");
        check (a->getHash() != c->getHash(), "different audio gives a different hash");

        const auto flac = SampleLoader::encodeFlac (*a);
        auto back = SampleLoader::loadEmbeddedFlac (flac, { "test", "", false });
        check (flac.getSize() > 0 && back.sample != nullptr, "a sample encodes to FLAC and decodes again");
        check (back.sample != nullptr && back.sample->getHash() == a->getHash() && back.sample->getLength() == a->getLength(),
               "the FLAC round trip is exact (same hash, same length)");
        check (back.sample != nullptr && back.sample->getEmbeddedFlac().getSize() == flac.getSize(), "a sample loaded from FLAC keeps that FLAC for saving");

        bool exact = back.sample != nullptr;
        for (int i = 0; exact && i < a->getLength(); ++i)
            exact = back.sample->getData()[i] == a->getData()[i];
        check (exact, "...sample for sample, including values above 0.5");
    }

    std::printf ("Files\n");
    {
        const auto file = juce::File::createTempFile (".wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (file);
            auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
            const auto b = sine (220.0, 0.75, 0.4f, 2, 44100.0);
            writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }

        auto result = SampleLoader::loadFile (file);
        check (result.sample != nullptr, "a 16-bit stereo WAV loads");
        if (result.sample != nullptr)
        {
            check (result.sample->getSampleRate() == 44100.0 && result.sample->getLength() == 33075, "it keeps its own sample rate and length");
            check (result.sample->getOrigin().name == file.getFileName() && result.sample->getOrigin().path == file.getFullPathName(), "name and path are recorded");
            check (result.sample->getEmbeddedFlac().getSize() > 0, "a short file gets an embedded FLAC copy for the saved state");
        }
        file.deleteFile();

        auto missing = SampleLoader::loadFile (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("no-such-neonsynth-sample.wav"));
        check (missing.sample == nullptr && missing.error.isNotEmpty(), "a missing file gives an error, not a sample");

        const auto junk = juce::File::createTempFile (".wav");
        junk.replaceWithText ("not audio");
        check (SampleLoader::loadFile (junk).sample == nullptr, "a file that isn't audio is refused");
        junk.deleteFile();
    }

    std::printf ("Default pad\n");
    {
        auto a = SampleLoader::makeDefaultSample();
        auto b = SampleLoader::makeDefaultSample();
        check (a != nullptr && a->getOrigin().isDefault && a->getEmbeddedFlac().getSize() == 0, "the pad is marked as the default and is never embedded");
        check (a->getHash() == b->getHash(), "the pad is identical every time it is made");

        // makeDefaultSample() hands out copies of one pad prepared once; each copy must be a
        // separate sample, equal to a freshly rendered one in every sample of every level.
        auto fresh = SampleLoader::renderDefaultSample();
        bool same = fresh->getHash() == a->getHash() && fresh->getOrigin().isDefault == a->getOrigin().isDefault
                 && fresh->getOrigin().name == a->getOrigin().name && fresh->getSampleRate() == a->getSampleRate();
        for (int level = 0; level < SampleData::numLevels && same; ++level)
        {
            same = fresh->getLength (level) == a->getLength (level);
            for (int i = -SampleData::padding; same && i < a->getLength (level) + SampleData::padding; ++i)
                same = fresh->getData (level)[i] == a->getData (level)[i];
        }
        check (same, "a copy of the prepared pad equals a freshly rendered one, sample for sample");
        a->users.fetch_add (1);
        check (a.get() != b.get() && a->getData() != b->getData() && b->users.load() == 0,
               "each copy is a separate sample with its own users count");
        a->users.fetch_sub (1);

        // Sessions that use the pad don't store it (they save default="1"), so it must stay the
        // same sound in every version. SampleLoader renders it with a frozen copy of the v1.5.0
        // oscillator, so improvements to the live one can't change it. These are its hashes in
        // v1.5.0, one per platform: it is computed with std::sin and std::tan, whose last bits
        // differ between MSVC's and glibc's maths libraries.
       #if JUCE_WINDOWS
        const juce::String v150PadHash = "9bbd2e8fe006ea2c";
       #elif JUCE_LINUX
        const juce::String v150PadHash = "bc3a2be832d2400d";
       #else
        const juce::String v150PadHash = a->getHash();   // no v1.5.0 reference recorded for this platform
       #endif
        char padWhat[160];
        std::snprintf (padWhat, sizeof (padWhat), "the pad is the same sound as in v1.5.0 (hash %s)", a->getHash().toRawUTF8());
        check (a->getHash() == v150PadHash, padWhat);
        check (std::abs (a->getLengthSeconds() - 4.0) < 1.0e-9, "the pad is 4 s long");

        const int n = a->getLength();
        const double atRoot = energyAt (a->getData() + n / 4, n / 2, 261.6256, rate);
        const double between = energyAt (a->getData() + n / 4, n / 2, 300.0, rate);
        check (atRoot > 100.0 * between, "the pad is pitched on C4 (261.6 Hz)");
    }

    std::printf ("Sample slot\n");
    {
        SampleSlot slot;
        slot.setAudioRunning (true);
        slot.publish (make (sine (440.0, 0.1, 0.3f)));
        const SampleData* first = slot.beginBlock();
        check (first != nullptr && slot.withCurrent ([&] (const SampleData* s) { return s == first; }), "the audio thread sees the published sample");

        first->users.fetch_add (1);              // a voice is still playing it
        slot.publish (make (sine (880.0, 0.1, 0.3f)));
        check (slot.getNumRetired() == 1, "a replaced sample isn't freed while the audio thread may still read it");

        const SampleData* second = slot.beginBlock();
        check (second != first, "the next block sees the new sample");
        slot.collectGarbage();
        check (slot.getNumRetired() == 1, "...and the old one stays while a voice still uses it");

        first->users.fetch_sub (1);
        slot.collectGarbage();
        check (slot.getNumRetired() == 0, "...and is freed once no voice uses it");

        slot.publish (make (sine (220.0, 0.1, 0.3f)));
        check (slot.getNumRetired() == 1, "with audio running, the block that read the old pointer must finish first");
        slot.setAudioRunning (false);
        check (slot.getNumRetired() == 0, "with audio stopped, a replaced sample is freed at once");
    }

    std::printf ("Sample slot under load\n");
    {
        SampleSlot slot;
        slot.setAudioRunning (true);
        slot.publish (make (sine (440.0, 0.05, 0.3f)));

        std::atomic<bool> stop { false };
        std::atomic<long> blocks { 0 };
        std::atomic<bool> sawBadData { false };

        // Audio thread: every block reads its sample; sometimes a "voice" holds one across blocks.
        std::thread audio ([&]
        {
            std::mt19937 rng (1);
            const SampleData* held = nullptr;
            int holdBlocks = 0;
            while (! stop.load())
            {
                const SampleData* s = slot.beginBlock();
                if (s != nullptr && (s->getLength() != 2400 || std::abs (s->getData()[s->getLength() / 2]) > 1.0f))
                    sawBadData = true;

                if (held == nullptr && s != nullptr && rng() % 4 == 0)
                {
                    held = s;
                    held->users.fetch_add (1);
                    holdBlocks = (int) (rng() % 20);
                }
                else if (held != nullptr && --holdBlocks <= 0)
                {
                    if (held->getLength() != 2400)
                        sawBadData = true;
                    held->users.fetch_sub (1);
                    held = nullptr;
                }
                ++blocks;
            }
            if (held != nullptr)
                held->users.fetch_sub (1);
        });

        for (int i = 0; i < 2000; ++i)
        {
            slot.publish (make (sine (440.0 + i, 0.05, 0.3f)));
            if (i % 50 == 0)
                std::this_thread::yield();
        }

        stop = true;
        audio.join();
        slot.setAudioRunning (false);

        char what[160];
        std::snprintf (what, sizeof (what), "2000 swaps during %ld audio blocks: the audio thread only ever read valid samples", blocks.load());
        check (! sawBadData.load(), what);
        check (slot.getNumRetired() == 0, "every replaced sample is freed in the end");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
