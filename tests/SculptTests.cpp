// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for Noise and Fold (Sculpt.h): both neutral settings change
    nothing, the noise is seeded and keeps the level steady, and the fold is
    anti-aliased, bounded and free of DC.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target SculptTests
    Run the resulting SculptTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include "../Sculpt.h"
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

    constexpr double rate = 48000.0;

    Sculpt make (SculptSettings settings, juce::uint32 seed = 1)
    {
        Sculpt s;
        s.set (settings);
        s.prepare (rate);   // starts at the settings, with no glide
        s.reseed (seed);
        return s;
    }

    std::vector<float> sine (double hz, int n, float amplitude = 1.0f, float offset = 0.0f)
    {
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = offset + amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate);
        return x;
    }

    std::vector<float> saw (double hz, int n)
    {
        std::vector<float> x ((size_t) n);
        double phase = 0.0;
        for (auto& v : x)
        {
            v = (float) (2.0 * phase - 1.0);
            phase += hz / rate;
            if (phase >= 1.0) phase -= 1.0;
        }
        return x;
    }

    double rms (const std::vector<float>& x, size_t from = 0)
    {
        double sum = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            sum += (double) x[i] * x[i];
        return std::sqrt (sum / (double) (x.size() - from));
    }

    void fft (std::vector<double>& re, std::vector<double>& im)
    {
        const size_t n = re.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double a = -juce::MathConstants<double>::twoPi / (double) len;
            for (size_t i = 0; i < n; i += len)
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const double c = std::cos (a * (double) k), s = std::sin (a * (double) k);
                    const double xr = re[i + k + len / 2] * c - im[i + k + len / 2] * s;
                    const double xi = re[i + k + len / 2] * s + im[i + k + len / 2] * c;
                    re[i + k + len / 2] = re[i + k] - xr;  im[i + k + len / 2] = im[i + k] - xi;
                    re[i + k] += xr;                       im[i + k] += xi;
                }
        }
    }

    /** Energy away from the harmonics of hz, relative to the total, in dB (as in OscillatorTests). */
    double aliasingDb (const std::vector<float>& x, double hz)
    {
        const size_t n = x.size();
        std::vector<double> re (n), im (n, 0.0);
        for (size_t i = 0; i < n; ++i)
        {
            const double p = juce::MathConstants<double>::twoPi * (double) i / (double) (n - 1);
            re[i] = x[i] * (0.35875 - 0.48829 * std::cos (p) + 0.14128 * std::cos (2 * p) - 0.01168 * std::cos (3 * p));
        }
        fft (re, im);
        double total = 0.0, off = 0.0;
        for (size_t k = 1; k < n / 2; ++k)
        {
            const double e = re[k] * re[k] + im[k] * im[k], f = (double) k * rate / (double) n;
            const double h = f / hz, nearest = std::round (h);
            total += e;
            if (nearest < 1.0 || std::abs (h - nearest) * hz > 6.0 * rate / (double) n)
                off += e;
        }
        return 10.0 * std::log10 (off / total);
    }
}

int main()
{
    char what[200];
    const auto input = saw (220.0, 48000);

    std::printf ("Neutral settings\n");
    {
        auto s = make ({ 0.0f, 7.0f, 0.0f });   // Noise 0, Fold Mix 0 (Drive irrelevant)
        bool same = true;
        for (const float x : input)
            same = same && s.process (x) == x;
        check (same, "Noise 0% and Fold Mix 0% return the input bit for bit");
    }

    std::printf ("Noise\n");
    {
        auto a = make ({ 0.5f, 3.0f, 0.0f }, 9), b = make ({ 0.5f, 3.0f, 0.0f }, 9), c = make ({ 0.5f, 3.0f, 0.0f }, 10);
        std::vector<float> x, y, z;
        for (const float v : input) { x.push_back (a.process (v)); y.push_back (b.process (v)); z.push_back (c.process (v)); }
        check (x == y && x != z, "the same seed renders bit-identical noise; another seed differs");

        double lowest = 1e9, highest = 0.0;
        for (const float level : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            auto s = make ({ level, 3.0f, 0.0f }, 3);
            std::vector<float> out;
            for (const float v : input)
                out.push_back (s.process (v));
            const double r = rms (out);
            lowest = std::min (lowest, r);
            highest = std::max (highest, r);
        }
        const double spread = 20.0 * std::log10 (highest / lowest);
        std::snprintf (what, sizeof (what), "the level holds steady from 0%% to 100%% noise (spread %.2f dB, under 1 dB)", spread);
        check (spread < 1.0, what);
    }

    std::printf ("Fold\n");
    {
        // A full-scale sine folded hard, against the same fold without anti-aliasing.
        const double hz = 1234.5;
        constexpr int n = 1 << 15;
        const auto x = sine (hz, n);
        auto s = make ({ 0.0f, 10.0f, 1.0f });
        std::vector<float> adaa (n), plain (n);
        for (int i = 0; i < n; ++i)
        {
            adaa[(size_t) i]  = s.process (x[(size_t) i]);
            plain[(size_t) i] = std::sin (juce::MathConstants<float>::halfPi * 10.0f * x[(size_t) i]);
        }
        const std::vector<float> settled (adaa.begin() + n / 4, adaa.end()), plainSettled (plain.begin() + n / 4, plain.end());
        std::vector<float> a2 (settled.begin(), settled.begin() + n / 2), p2 (plainSettled.begin(), plainSettled.begin() + n / 2);
        const double before = aliasingDb (p2, hz), after = aliasingDb (a2, hz);
        std::snprintf (what, sizeof (what), "Drive 10 on a 1.23 kHz sine: aliasing %.1f dB, plain fold %.1f dB (at least 10 dB lower)", after, before);
        check (after <= before - 10.0, what);

        float peak = 0.0f;
        for (const float v : settled)
            peak = std::max (peak, std::abs (v));
        std::snprintf (what, sizeof (what), "the folded output stays bounded (peak %.3f, under 1.05)", peak);
        check (peak < 1.05f, what);

        // An offset input folds into an offset output; the DC blocker removes it.
        const auto offsetInput = sine (220.0, 96000, 0.6f, 0.4f);
        auto d = make ({ 0.0f, 4.0f, 1.0f });
        std::vector<float> out;
        for (const float v : offsetInput)
            out.push_back (d.process (v));
        double mean = 0.0;
        for (size_t i = 48000; i < out.size(); ++i)
            mean += out[i];
        mean /= 48000.0;
        const double dcDb = 20.0 * std::log10 (std::abs (mean) / rms (out, 48000) + 1e-12);
        std::snprintf (what, sizeof (what), "DC after folding an offset wave: %.1f dB below the signal (at least 60 dB)", -dcDb);
        check (dcDb < -60.0, what);
    }

    std::printf ("Dry and wet line up\n");
    {
        // Fold Mix 50% with a gentle drive on a quiet sine: the fold is nearly a plain gain there,
        // so if the dry path weren't delayed to match the oversampled wet one, the mix would
        // comb-filter (notches every 3.2 kHz). The level must instead be smooth across the band.
        double lowest = 1e9, highest = 0.0;
        for (const double hz : { 200.0, 1100.0, 1600.0, 3200.0, 4800.0, 8000.0, 12000.0 })
        {
            auto s = make ({ 0.0f, 1.0f, 0.5f });
            const auto x = sine (hz, 24000, 0.05f);
            std::vector<float> out;
            for (const float v : x)
                out.push_back (s.process (v));
            const double gain = rms (out, 4800) / rms (x, 4800);
            lowest = std::min (lowest, gain);
            highest = std::max (highest, gain);
        }
        const double ripple = 20.0 * std::log10 (highest / lowest);
        std::snprintf (what, sizeof (what), "at 50%% mix the response is flat from 200 Hz to 12 kHz within %.2f dB (under 1.5 dB)", ripple);
        check (ripple < 1.5, what);
    }

    std::printf ("Fast sine\n");
    {
        double worst = 0.0;
        for (int i = -100000; i <= 100000; ++i)
        {
            const float x = (float) i * 0.0002f;   // -20 .. 20
            worst = std::max (worst, (double) std::abs (Sculpt::fastSin (x) - std::sin (x)));
        }
        std::snprintf (what, sizeof (what), "fastSin matches std::sin from -20 to 20 within %.2g (under 1e-5)", worst);
        check (worst < 1e-5, what);
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
