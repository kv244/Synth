// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for PolyBlepOscillator, the voice's band-limited oscillator.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target OscillatorTests
    Run the resulting OscillatorTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include "../Oscillator.h"
#include <cstdio>
#include <cmath>

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

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
