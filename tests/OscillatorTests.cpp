// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for the voice's band-limited oscillators: PolyBlepOscillator
    and RandomStepOscillator (the Random source).

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target OscillatorTests
    Run the resulting OscillatorTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include "../Oscillator.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    constexpr double testSampleRate = 48000.0;
    const char* names[] = { "sine", "square", "triangle", "sawtooth" };

    /** Oscillator already running the given waveform (a waveform change waits for a cycle boundary). */
    PolyBlepOscillator make (int waveform, float hz)
    {
        PolyBlepOscillator osc;
        osc.setWaveform (waveform);
        osc.setFrequency (hz, testSampleRate);
        while (osc.getWaveform() != waveform)
            osc.nextSample();
        return osc;
    }
}

int main()
{
    std::printf ("Pitch\n");
    for (int w = 0; w < 4; ++w)
    {
        auto osc = make (w, 440.0f);
        int crossings = 0;
        float previous = osc.nextSample();
        double sum = 0.0;
        for (int i = 1; i < (int) testSampleRate; ++i)
        {
            const float x = osc.nextSample();
            if (previous < 0.0f && x >= 0.0f)
                ++crossings;
            previous = x;
            sum += x;
        }
        char what[128];
        std::snprintf (what, sizeof (what), "%s at 440 Hz: %d rising zero crossings in 1 s", names[w], crossings);
        check (std::abs (crossings - 440) <= 1, what);
        std::snprintf (what, sizeof (what), "%s has no DC offset (mean %.5f)", names[w], sum / testSampleRate);
        check (std::abs (sum / testSampleRate) < 0.01, what);
    }

    std::printf ("Level\n");
    for (int w = 0; w < 4; ++w)
    {
        auto osc = make (w, 5000.0f);   // high pitch: the PolyBLEP corrections are at their largest
        float peak = 0.0f;
        for (int i = 0; i < (int) testSampleRate; ++i)
            peak = std::max (peak, std::abs (osc.nextSample()));
        char what[128];
        std::snprintf (what, sizeof (what), "%s at 5 kHz peaks at %.3f (between 0.75 and 1.1)", names[w], peak);
        check (peak > 0.75f && peak < 1.1f, what);   // PolyBLEP rounds the saw peak at high pitch
    }

    std::printf ("Band limiting\n");
    {
        // A naive sawtooth steps by 2 at every wrap; PolyBLEP spreads the step over two samples.
        auto osc = make (PolyBlepOscillator::sawtooth, 2000.0f);
        float previous = osc.nextSample(), largestStep = 0.0f;
        for (int i = 0; i < 4800; ++i)
        {
            const float x = osc.nextSample();
            largestStep = std::max (largestStep, std::abs (x - previous));
            previous = x;
        }
        check (largestStep < 1.9f, "sawtooth step at the wrap is smoothed (largest sample-to-sample step under 1.9)");
    }

    std::printf ("Waveform changes\n");
    {
        // 480 Hz at 48 kHz: exactly 100 samples a cycle. Ask for a sawtooth 30 samples into a square cycle.
        auto osc = make (PolyBlepOscillator::square, 480.0f);
        for (int i = 0; i < 30; ++i)
            osc.nextSample();
        osc.setWaveform (PolyBlepOscillator::sawtooth);

        int squareSamples = 0;
        while (osc.getWaveform() == PolyBlepOscillator::square && squareSamples < 1000)
        {
            osc.nextSample();
            ++squareSamples;
        }
        check (std::abs (squareSamples - 70) <= 1, "a waveform change waits for the end of the current cycle");
        check (osc.getWaveform() == PolyBlepOscillator::sawtooth, "the new waveform takes over at the cycle boundary");
    }

    std::printf ("Pitch change\n");
    {
        auto osc = make (PolyBlepOscillator::sine, 440.0f);
        for (int i = 0; i < 1234; ++i)
            osc.nextSample();
        const float before = osc.nextSample();
        osc.setFrequency (880.0f, testSampleRate);
        const float after = osc.nextSample();
        check (std::abs (after - before) < 0.12f, "a pitch change keeps the phase (no jump in the waveform)");
    }

    // ── Random source ───────────────────────────────────────────────────
    const auto randomAt = [] (float hz, juce::uint32 seed)
    {
        RandomStepOscillator r;
        r.reseed (seed);
        r.setFrequency (hz, testSampleRate);
        return r;
    };
    const auto renderRandom = [] (RandomStepOscillator& r, int n)
    {
        std::vector<float> out ((size_t) n);
        for (auto& x : out)
            x = r.nextSample();
        return out;
    };

    std::printf ("Random: repeatability and range\n");
    {
        auto a = randomAt (220.0f, 7), b = randomAt (220.0f, 7), c = randomAt (220.0f, 8);
        const auto x = renderRandom (a, 48000), y = renderRandom (b, 48000), z = renderRandom (c, 48000);
        check (x == y, "the same seed renders bit-identical audio");
        check (x != z, "a different seed renders a different wave");

        for (const float hz : { 55.0f, 1000.0f, 5000.0f, 15000.0f })
        {
            auto r = randomAt (hz, 3);
            float peak = 0.0f;
            for (int i = 0; i < 48000; ++i)
                peak = std::max (peak, std::abs (r.nextSample()));
            char what[128];
            std::snprintf (what, sizeof (what), "at %.0f Hz the output stays within -1..1 (peak %.3f)", hz, peak);
            check (peak <= 1.0f, what);
        }
    }

    std::printf ("Random: step statistics\n");
    {
        // 375 Hz at 48 kHz: exactly 128 samples a cycle, 8 per step.
        auto r = randomAt (375.0f, 11);
        double sum = 0.0, sumSquares = 0.0;
        int count = 0, mostChangedInACycle = 0, cyclesWithAChange = 0;
        auto previous = r.getLevels();
        for (int cycle = 0; cycle < 4000; ++cycle)
        {
            for (int i = 0; i < 128; ++i)
                r.nextSample();
            const auto& levels = r.getLevels();
            int changed = 0;
            for (int k = 0; k < RandomStepOscillator::numSteps; ++k)
            {
                changed += levels[(size_t) k] != previous[(size_t) k] ? 1 : 0;
                sum += levels[(size_t) k];
                sumSquares += (double) levels[(size_t) k] * levels[(size_t) k];
                ++count;
            }
            mostChangedInACycle = std::max (mostChangedInACycle, changed);
            cyclesWithAChange += changed > 0 ? 1 : 0;
            previous = levels;
        }
        const double mean = sum / count, rmsLevel = std::sqrt (sumSquares / count);
        char what[160];
        std::snprintf (what, sizeof (what), "levels are uniform in -1..1 (mean %.3f, RMS %.3f; expected 0 and 0.577)", mean, rmsLevel);
        check (std::abs (mean) < 0.03 && std::abs (rmsLevel - 0.5774) < 0.02, what);
        std::snprintf (what, sizeof (what), "at most one level is replaced per cycle (most seen %d), about 16 of every 17 cycles (%d of 4000)",
                       mostChangedInACycle, cyclesWithAChange);
        check (mostChangedInACycle == 1 && std::abs (cyclesWithAChange - 4000 * 16 / 17) < 20, what);
    }

    std::printf ("Random: pitch\n");
    {
        // 240 Hz: exactly 200 samples a cycle. A pitched wave repeats itself one period later;
        // sample-and-hold noise (every level re-rolled every cycle) would not.
        auto r = randomAt (240.0f, 5);
        const auto x = renderRandom (r, 96000);
        const auto correlation = [&] (int lag)
        {
            double xy = 0.0, xx = 0.0;
            for (size_t i = 0; i + (size_t) lag < x.size(); ++i)
            {
                xy += (double) x[i] * x[i + (size_t) lag];
                xx += (double) x[i] * x[i];
            }
            return xy / xx;
        };
        const double onePeriod = correlation (200), halfStep = correlation (200 + 6);
        char what[160];
        std::snprintf (what, sizeof (what), "the wave repeats one period later (correlation %.2f at 1 period, %.2f off it)", onePeriod, halfStep);
        check (onePeriod > 0.85 && onePeriod > halfStep + 0.3, what);
    }

    std::printf ("Random: band limiting\n");
    {
        // The same stream rendered naively (a level held for each step, hard edges), by tracking
        // the oscillator's phase with the same float steps. The PolyBLEP edges must leave
        // much less energy near Nyquist, where most of the aliasing lands.
        const float hz = 440.25f;
        auto r = randomAt (hz, 9);
        const float inc = (float) (hz / testSampleRate);
        constexpr int n = 8192;
        std::vector<float> smooth ((size_t) n), naive ((size_t) n);
        float phase = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const int step = std::min ((int) (phase * (float) RandomStepOscillator::numSteps), RandomStepOscillator::numSteps - 1);
            smooth[(size_t) i] = r.nextSample();
            naive[(size_t) i]  = r.getLevels()[(size_t) step];   // the re-rolled level is never the current one
            phase += inc;
            if (phase >= 1.0f)
                phase -= 1.0f;
        }
        const auto bandEnergy = [&] (const std::vector<float>& x, double fromHz, double toHz)
        {
            double total = 0.0;
            for (int k = (int) (fromHz * n / testSampleRate); k < (int) (toHz * n / testSampleRate); ++k)
            {
                double re = 0.0, im = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (n - 1));   // Hann
                    const double angle = juce::MathConstants<double>::twoPi * (double) k * i / n;
                    re += w * x[(size_t) i] * std::cos (angle);
                    im -= w * x[(size_t) i] * std::sin (angle);
                }
                total += re * re + im * im;
            }
            return total;
        };
        const double gain = 10.0 * std::log10 (bandEnergy (naive, 18000.0, 24000.0) / bandEnergy (smooth, 18000.0, 24000.0));
        char what[160];
        std::snprintf (what, sizeof (what), "PolyBLEP edges cut the energy at 18-24 kHz by %.1f dB against hard steps (at least 6 dB)", gain);
        check (gain > 6.0, what);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
