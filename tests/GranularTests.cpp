// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for GrainCloud, the granular source: determinism, density,
    steady level across size and density, pitch, the 32-grain cap, scan,
    retrigger without clicks, and the reference it holds on the sample.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target GranularTests
    Run the resulting GranularTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include "../SampleData.h"
#include "../GrainCloud.h"
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

    constexpr double rate = 48000.0;

    std::unique_ptr<SampleData> noiseSample (double seconds)
    {
        juce::AudioBuffer<float> b (1, (int) (seconds * rate));
        std::mt19937 rng (3);
        std::uniform_real_distribution<float> dist (-0.5f, 0.5f);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, dist (rng));
        juce::String error;
        return SampleData::create (b, rate, { "noise", {}, false }, error);
    }

    std::unique_ptr<SampleData> sineSample (double hz, double seconds)
    {
        juce::AudioBuffer<float> b (1, (int) (seconds * rate));
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate));
        juce::String error;
        return SampleData::create (b, rate, { "sine", {}, false }, error);
    }

    GrainCloud::Settings cloud (float sizeMs, float density, float spray = 0.5f, float jitter = 0.0f, float scan = 0.0f)
    {
        GrainCloud::Settings s;
        s.position = 0.5f;
        s.sizeSeconds = sizeMs * 0.001f;
        s.density = density;
        s.spray = spray;
        s.jitterCents = jitter;
        s.scan = scan;
        return s;
    }

    std::vector<float> render (GrainCloud& c, int n)
    {
        std::vector<float> out ((size_t) n);
        for (auto& x : out)
            x = c.nextSample();
        return out;
    }

    double rms (const std::vector<float>& x, size_t from)
    {
        double sum = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            sum += (double) x[i] * x[i];
        return std::sqrt (sum / (double) (x.size() - from));
    }

    double energyAt (const std::vector<float>& x, double hz)
    {
        const double w = juce::MathConstants<double>::twoPi * hz / rate, c = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;
        for (const float v : x)
        {
            const double s0 = v + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    }
}

int main()
{
    char what[200];
    auto noise = noiseSample (2.0);

    std::printf ("Determinism\n");
    {
        GrainCloud a, b, c;
        a.begin (noise.get(), 60, 60, rate, cloud (60, 40, 0.5f, 30.0f, 0.3f), 1234);
        b.begin (noise.get(), 60, 60, rate, cloud (60, 40, 0.5f, 30.0f, 0.3f), 1234);
        c.begin (noise.get(), 60, 60, rate, cloud (60, 40, 0.5f, 30.0f, 0.3f), 999);
        const auto x = render (a, 96000), y = render (b, 96000), z = render (c, 96000);
        check (x == y, "the same seed renders bit-identical audio");
        check (x != z, "a different seed renders different grains");
        a.end(); b.end(); c.end();
    }

    std::printf ("Density\n");
    for (const float density : { 5.0f, 50.0f, 200.0f })
    {
        GrainCloud g;
        g.begin (noise.get(), 60, 60, rate, cloud (5, density), 42);
        render (g, 480000);   // 10 s
        const auto expected = (double) density * 10.0;
        std::snprintf (what, sizeof (what), "%.0f grains/s for 10 s starts %lld grains (expected %.0f +-5%%)", density, (long long) g.getGrainsStarted(), expected);
        check (std::abs ((double) g.getGrainsStarted() - expected) <= 0.05 * expected + 1.0, what);
        g.end();
    }

    std::printf ("Level\n");
    {
        double lowest = 1e9, highest = 0.0;
        for (const float size : { 30.0f, 100.0f, 300.0f })
            for (const float density : { 10.0f, 40.0f, 100.0f })
            {
                if (size * 0.001f * density < 1.0f)
                    continue;   // thinner clouds have gaps between grains: quieter by design
                GrainCloud g;
                g.begin (noise.get(), 60, 60, rate, cloud (size, density, 1.0f), 7);
                const auto out = render (g, 192000);
                const double level = rms (out, 48000);   // after the cloud has filled up
                lowest = std::min (lowest, level);
                highest = std::max (highest, level);
                g.end();
            }
        const double spread = juce::Decibels::gainToDecibels (highest / lowest);
        std::snprintf (what, sizeof (what), "the level stays within +-3 dB from 30 to 300 ms and 10 to 100 grains/s (spread %.1f dB)", spread);
        check (spread < 6.0, what);
    }

    std::printf ("Pitch\n");
    {
        auto sine = sineSample (440.0, 2.0);
        for (const int note : { 60, 72 })
        {
            GrainCloud g;
            g.begin (sine.get(), note, 60, rate, cloud (80, 30, 0.3f), 11);
            const auto out = render (g, 96000);
            const double expected = 440.0 * std::pow (2.0, (note - 60) / 12.0);
            const double onPitch = energyAt (out, expected), off = energyAt (out, expected * 1.12);
            std::snprintf (what, sizeof (what), "grains of a 440 Hz sample at +%d semitones sound at %.0f Hz (%.0f dB above a nearby frequency)",
                           note - 60, expected, 10.0 * std::log10 (onPitch / off));
            check (onPitch > 100.0 * off, what);
            g.end();
        }
    }

    std::printf ("Grain cap\n");
    {
        GrainCloud g;
        g.begin (noise.get(), 60, 60, rate, cloud (500, 200), 5);   // wants 100 grains at once
        int most = 0;
        for (int i = 0; i < 96000; ++i)
        {
            g.nextSample();
            most = std::max (most, g.getNumActiveGrains());
        }
        std::snprintf (what, sizeof (what), "never more than %d grains at once (most seen %d); further onsets are skipped", GrainCloud::maxGrains, most);
        check (most == GrainCloud::maxGrains, what);
        g.end();
    }

    std::printf ("Scan\n");
    {
        // Spray 0: every grain starts at the scan head. With Scan +1 the head moves at the file's
        // own speed, so the grains' positions move by about 1 s of file per second.
        // The cloud's position: render on until a grain is sounding, then read it.
        const auto cloudPosition = [] (GrainCloud& c)
        {
            float p[GrainCloud::maxGrains];
            for (int i = 0; i < 48000; ++i)
            {
                if (c.getGrainPositions (p, 1) == 1)
                    return p[0];
                c.nextSample();
            }
            return -1.0f;
        };

        GrainCloud g;
        g.begin (noise.get(), 60, 60, rate, cloud (20, 50, 0.0f, 0.0f, 1.0f), 9);
        render (g, 4800);
        const float before = cloudPosition (g);
        render (g, 24000);   // 0.5 s: a quarter of the 2 s file
        const float moved = cloudPosition (g) - before;
        std::snprintf (what, sizeof (what), "Scan +1 moves the cloud through a 2 s file at its own speed (0.5 s moved it %.3f of the file, expected 0.25)", moved);
        check (std::abs (moved - 0.25f) < 0.03f, what);
        g.end();

        g.begin (noise.get(), 60, 60, rate, cloud (20, 50, 0.0f, 0.0f, 0.0f), 9);
        render (g, 4800);
        const float frozen = cloudPosition (g);
        render (g, 48000);
        const float later = cloudPosition (g);
        std::snprintf (what, sizeof (what), "Scan 0 freezes the cloud in place (%.3f, then %.3f a second later)", frozen, later);
        check (frozen >= 0.0f && std::abs (later - frozen) < 0.02f, what);
        g.end();
    }

    std::printf ("Retrigger and references\n");
    {
        GrainCloud g;
        g.begin (noise.get(), 60, 60, rate, cloud (100, 40), 1);
        render (g, 24000);
        const int sounding = g.getNumActiveGrains();
        g.begin (noise.get(), 67, 60, rate, cloud (100, 40), 2);   // retrigger at another pitch
        check (sounding > 0 && g.getNumActiveGrains() == sounding, "a retrigger keeps the sounding grains (they fade out by their window: no click)");
        check (noise->users.load() == 1, "one reference on the sample while the cloud plays");

        g.stopSpawning();
        int n = 0;
        while (g.isSounding() && n < 96000) { g.nextSample(); ++n; }
        check (! g.isSounding() && n <= 4800 + 2, "after stopSpawning() the last grains end within one grain length");
        g.end();
        g.end();
        check (noise->users.load() == 0, "end() releases the sample, and calling it twice is harmless");
        check (! g.begin (nullptr, 60, 60, rate, cloud (80, 30), 1) && ! g.isActive(), "no sample: begin() refuses and plays nothing");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
