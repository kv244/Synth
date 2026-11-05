// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Render benchmark: times NeonSynthProcessor::processBlock directly,
    with no host or Python in the way, so the numbers are the plugin's own cost.

    Scenarios (48 kHz, 512-sample blocks, 60 s of audio each):
      idle      - no notes
      one note  - a single held note
      16 voices - 16 notes held at full velocity (soft clipper working hard)
      sample 16 - the same 16 notes playing the built-in pad (Source = Sample, looped)
      dense     - sequencer at 1/32 and 240 BPM plus an 8-note chord every 250 ms

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --config Release --target RenderBenchmark
    Prints, per scenario, the render time and the share of one CPU core used
    in real time.
*/
#include "NeonSynthProcessor.h"
#include <chrono>
#include <cstdio>
#include <functional>

namespace
{
    constexpr double benchSampleRate = 48000.0;
    constexpr int    benchBlockSize  = 512;
    constexpr double benchSeconds    = 60.0;

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    /** Renders `benchSeconds` of audio; `addMidi` fills each block's MIDI. Returns milliseconds spent in processBlock. */
    double render (const std::function<void (juce::MidiBuffer&, juce::int64 blockStart)>& addMidi,
                   const std::function<void (juce::AudioProcessor&)>& configure)
    {
        NeonSynthProcessor processor;
        configure (processor);
        processor.setRateAndBufferSizeDetails (benchSampleRate, benchBlockSize);
        processor.prepareToPlay (benchSampleRate, benchBlockSize);

        juce::AudioBuffer<float> buffer (2, benchBlockSize);
        juce::MidiBuffer midi;
        double total = 0.0;
        float peak = 0.0f;

        const auto blocks = (juce::int64) (benchSeconds * benchSampleRate / benchBlockSize);
        for (juce::int64 b = 0; b < blocks; ++b)
        {
            midi.clear();
            addMidi (midi, b * benchBlockSize);
            const auto start = std::chrono::steady_clock::now();
            processor.processBlock (buffer, midi);
            total += std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
            peak = juce::jmax (peak, buffer.getMagnitude (0, benchBlockSize));
        }
        processor.releaseResources();
        std::printf ("        (output peak %.1f dBFS)\n", juce::Decibels::gainToDecibels (peak));
        return total;
    }

    void report (const char* name, double ms)
    {
        std::printf ("  %-10s %8.0f ms for %.0f s   %5.2f%% of one core\n", name, ms, benchSeconds, 100.0 * ms / (benchSeconds * 1000.0));
    }

    /** Adds the events of a note that is held for the whole render (note-on in the first block only). */
    std::function<void (juce::MidiBuffer&, juce::int64)> held (std::initializer_list<int> notes, int velocity)
    {
        std::vector<int> list (notes);
        return [list, velocity] (juce::MidiBuffer& midi, juce::int64 blockStart)
        {
            if (blockStart == 0)
                for (int n : list)
                    midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) velocity), 0);
        };
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;   // the processor's editor code needs the message manager
    const auto noChange = [] (juce::AudioProcessor&) {};

    std::printf ("Render benchmark (48 kHz, %d-sample blocks)\n", benchBlockSize);

    report ("idle",     render ([] (juce::MidiBuffer&, juce::int64) {}, noChange));
    report ("one note", render (held ({ 60 }, 100), noChange));
    report ("16 voices", render (held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127), noChange));
    report ("sample 16", render (held ({ 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74 }, 127),
                                 [] (juce::AudioProcessor& p) { setParameter (p, "source", 1.0f); }));   // the built-in pad, looped

    const auto dense = [] (juce::MidiBuffer& midi, juce::int64 blockStart)
    {
        // An 8-note chord every 250 ms (12000 samples), each note 200 ms long.
        for (juce::int64 s = blockStart; s < blockStart + benchBlockSize; ++s)
        {
            const auto inChord = s % 12000;
            for (int k = 0; k < 8; ++k)
            {
                if (inChord == k * 144)        midi.addEvent (juce::MidiMessage::noteOn  (1, 48 + 3 * k, (juce::uint8) 100), (int) (s - blockStart));
                if (inChord == 9600 + k * 144) midi.addEvent (juce::MidiMessage::noteOff (1, 48 + 3 * k), (int) (s - blockStart));
            }
        }
    };
    report ("dense", render (dense, [] (juce::AudioProcessor& p)
    {
        setParameter (p, "seqOn", 1.0f);
        setParameter (p, "seqSync", 0.0f);
        setParameter (p, "seqRate", 2.0f);      // 1/32
        setParameter (p, "seqTempo", 240.0f);
    }));
    return 0;
}
