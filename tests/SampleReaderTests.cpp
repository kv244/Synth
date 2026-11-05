// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for SampleReader, which plays a note from a sample: pitch
    tracking, sample-rate conversion, the band-limited copies used for high
    notes, crossfaded loops, one-shot ends, Loop until release, and the
    reference it holds on the sample.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target SampleReaderTests
    Run the resulting SampleReaderTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include "../SampleData.h"
#include "../SampleReader.h"
#include <cstdio>
#include <cmath>
#include <random>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    constexpr double hostRate = 48000.0;

    std::unique_ptr<SampleData> sineSample (double hz, double seconds, double rate = hostRate)
    {
        juce::AudioBuffer<float> b (1, (int) (seconds * rate));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate));
        juce::String error;
        return SampleData::create (b, rate, { "sine", {}, false }, error);
    }

    std::vector<float> render (SampleReader& r, int n)
    {
        std::vector<float> out ((size_t) n);
        for (auto& x : out)
            x = r.nextSample();
        return out;
    }

    /** Frequency from rising zero crossings, skipping the first and last 10%. */
    double frequencyOf (const std::vector<float>& x, double rate)
    {
        const size_t from = x.size() / 10, to = x.size() - x.size() / 10;
        int crossings = 0;
        size_t first = 0, last = 0;
        for (size_t i = from + 1; i < to; ++i)
        {
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            {
                if (crossings == 0) first = i;
                last = i;
                ++crossings;
            }
        }
        return crossings > 1 ? (crossings - 1) * rate / (double) (last - first) : 0.0;
    }

    double rms (const std::vector<float>& x)
    {
        double sum = 0.0;
        for (size_t i = x.size() / 10; i < x.size() - x.size() / 10; ++i)
            sum += (double) x[i] * x[i];
        return std::sqrt (sum / (double) (x.size() - 2 * (x.size() / 10)));
    }

    float largestStep (const std::vector<float>& x)
    {
        float step = 0.0f;
        for (size_t i = 1; i < x.size(); ++i)
            step = std::max (step, std::abs (x[i] - x[i - 1]));
        return step;
    }

    SampleReader::Settings settings (int root, int mode, float loopStart = 0.0f, float loopEnd = 1.0f, float xfadeMs = 10.0f)
    {
        SampleReader::Settings s;
        s.rootNote = root;
        s.loopMode = mode;
        s.loopStart = loopStart;
        s.loopEnd = loopEnd;
        s.crossfadeSeconds = xfadeMs * 0.001f;
        return s;
    }
}

int main()
{
    char what[200];

    std::printf ("Pitch tracking\n");
    {
        auto sample = sineSample (440.0, 2.0);   // A4 recorded at A4
        for (const int note : { 57, 69, 81 })     // A3, A4, A5
        {
            SampleReader r;
            r.begin (sample.get(), note, hostRate, settings (69, SampleReader::loop));
            const double expected = 440.0 * std::pow (2.0, (note - 69) / 12.0);
            const double got = frequencyOf (render (r, 48000), hostRate);
            std::snprintf (what, sizeof (what), "note %d plays at %.2f Hz (expected %.2f, within 0.1%%)", note, got, expected);
            check (std::abs (got / expected - 1.0) < 0.001, what);
            r.end();
        }

        auto at44k = sineSample (440.0, 2.0, 44100.0);   // file at 44.1 kHz, host at 48 kHz
        SampleReader r;
        r.begin (at44k.get(), 69, hostRate, settings (69, SampleReader::loop));
        const double got = frequencyOf (render (r, 48000), hostRate);
        std::snprintf (what, sizeof (what), "a 44.1 kHz file at a 48 kHz host keeps its pitch (%.2f Hz)", got);
        check (std::abs (got / 440.0 - 1.0) < 0.001, what);
        r.end();
    }

    std::printf ("Band-limited copies\n");
    {
        SampleReader r;
        auto sample = sineSample (1000.0, 1.0);
        r.begin (sample.get(), 62, hostRate, settings (60, SampleReader::oneShot));
        check (r.getLevel() == 0, "2 semitones up reads the full-rate buffer (keeps the top end)");
        r.end();
        r.begin (sample.get(), 72, hostRate, settings (60, SampleReader::oneShot));
        check (r.getLevel() == 1 && std::abs (r.getIncrement() - 1.0) < 1e-9, "12 semitones up reads the 1/2-rate copy at unity increment");
        r.end();
        r.begin (sample.get(), 84, hostRate, settings (60, SampleReader::oneShot));
        check (r.getLevel() == 2, "24 semitones up reads the 1/4-rate copy");
        r.end();

        // +24 semitones: a 9 kHz tone would land at 36 kHz, above Nyquist, and fold back. It must be gone.
        auto high = sineSample (9000.0, 1.0);
        auto low  = sineSample (1000.0, 1.0);
        r.begin (high.get(), 84, hostRate, settings (60, SampleReader::loop));
        const double aliased = rms (render (r, 24000));
        r.end();
        r.begin (low.get(), 84, hostRate, settings (60, SampleReader::loop));
        const double reference = rms (render (r, 24000));
        r.end();
        const double db = juce::Decibels::gainToDecibels (aliased / reference);
        std::snprintf (what, sizeof (what), "+24 semitones: a 9 kHz tone (36 kHz after pitching) is %.1f dB below a 1 kHz one: no aliasing", db);
        check (db < -60.0, what);

        // +2 semitones: an 8 kHz tone becomes 9 kHz and must pass at (nearly) full level.
        auto bright = sineSample (8000.0, 1.0);
        r.begin (bright.get(), 62, hostRate, settings (60, SampleReader::loop));
        const std::vector<float> brightSource (bright->getData(), bright->getData() + bright->getLength());
        const double brightDb = juce::Decibels::gainToDecibels (rms (render (r, 24000)) / rms (brightSource));
        r.end();
        std::snprintf (what, sizeof (what), "+2 semitones keeps an 8 kHz tone (now 9 kHz) within 1 dB (%.2f dB)", brightDb);
        check (std::abs (brightDb) < 1.0, what);
    }

    std::printf ("Loops\n");
    {
        // White noise, looped over 20% - 60% with a 10 ms crossfade, for 10 s.
        juce::AudioBuffer<float> noise (1, 48000);
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> dist (-0.5f, 0.5f);
        for (int i = 0; i < noise.getNumSamples(); ++i)
            noise.setSample (0, i, dist (rng));
        juce::String error;
        auto sample = SampleData::create (noise, hostRate, {}, error);

        SampleReader r;
        r.begin (sample.get(), 60, hostRate, settings (60, SampleReader::loop, 0.2f, 0.6f, 10.0f));
        const auto out = render (r, 480000);

        // Equal-power crossfade: the level holds steady through every loop point (5 ms windows).
        double lowest = 1.0e9, highest = 0.0;
        for (size_t w = 0; w + 240 <= out.size(); w += 240)
        {
            double sum = 0.0;
            for (size_t i = w; i < w + 240; ++i)
                sum += (double) out[i] * out[i];
            lowest = std::min (lowest, sum);
            highest = std::max (highest, sum);
        }
        const double spreadDb = 10.0 * std::log10 (highest / lowest);
        std::snprintf (what, sizeof (what), "a looped noise sample keeps a steady level through 25 loop points (5 ms windows within %.1f dB)", spreadDb);
        check (spreadDb < 3.0, what);
        check (! r.hasFinished(), "a looped note never finishes on its own");
        r.end();

        // A sine looped over a non-integer number of cycles: the crossfade hides the seam.
        auto sine = sineSample (441.3, 1.0);
        r.begin (sine.get(), 60, hostRate, settings (60, SampleReader::loop, 0.1f, 0.5f, 20.0f));
        const auto looped = render (r, 96000);
        const float sineStep = 0.89125f * 2.0f * (float) std::sin (juce::MathConstants<double>::pi * 441.3 / hostRate);
        std::snprintf (what, sizeof (what), "a sine looped off-cycle stays smooth: largest step %.4f (a clean sine's is %.4f)", largestStep (looped), sineStep);
        check (largestStep (looped) < sineStep * 1.5f, what);
        r.end();

        // Loop points too close together are widened, not fatal.
        r.begin (sine.get(), 60, hostRate, settings (60, SampleReader::loop, 0.5f, 0.5f));
        render (r, 1000);
        check (r.isActive() && ! r.hasFinished(), "an empty loop (start = end) still plays");
        r.end();
    }

    std::printf ("One-shot and Loop until release\n");
    {
        auto sample = sineSample (440.0, 0.25);   // 12000 samples
        SampleReader r;
        r.begin (sample.get(), 72, hostRate, settings (60, SampleReader::oneShot));   // an octave up: half the time
        int n = 0;
        while (! r.hasFinished() && n < 100000) { r.nextSample(); ++n; }
        std::snprintf (what, sizeof (what), "a one-shot an octave up finishes after %d samples (expected 6000)", n);
        check (std::abs (n - 6000) <= 2, what);
        check (r.nextSample() == 0.0f, "after the end it outputs silence");
        r.end();

        r.begin (sample.get(), 60, hostRate, settings (60, SampleReader::loopUntilRelease, 0.2f, 0.8f, 5.0f));
        render (r, 48000);   // 4x the sample's length
        check (! r.hasFinished(), "Loop until release keeps looping while the key is held");
        r.keyReleased();
        n = 0;
        while (! r.hasFinished() && n < 100000) { r.nextSample(); ++n; }
        check (r.hasFinished() && n <= 12000, "...and after release plays on to the end and finishes");
        r.end();

        r.begin (sample.get(), 60, hostRate, settings (60, SampleReader::oneShot));
        r.end();
        r.begin (sample.get(), 60, hostRate, { 60, 0.5f, SampleReader::oneShot, 0.0f, 1.0f, 0.01f });
        n = 0;
        while (! r.hasFinished() && n < 100000) { r.nextSample(); ++n; }
        check (std::abs (n - 6000) <= 2, "Start at 50% plays the second half");
        r.end();
    }

    std::printf ("Sample reference\n");
    {
        auto sample = sineSample (440.0, 0.1);
        SampleReader a, b;
        a.begin (sample.get(), 60, hostRate, settings (60, SampleReader::loop));
        b.begin (sample.get(), 64, hostRate, settings (60, SampleReader::loop));
        check (sample->users.load() == 2, "each playing note holds one reference on the sample");
        a.swapWith (b);
        check (sample->users.load() == 2, "swapping two readers keeps the count");
        a.begin (sample.get(), 67, hostRate, settings (60, SampleReader::loop));
        check (sample->users.load() == 2, "starting a new note on a reader releases its old reference");
        a.end();
        b.end();
        b.end();
        check (sample->users.load() == 0, "end() releases it, and calling it twice is harmless");
        check (! a.begin (nullptr, 60, hostRate, settings (60, SampleReader::loop)) && ! a.isActive(), "no sample: begin() refuses and plays nothing");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
