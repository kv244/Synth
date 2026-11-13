// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Real-time safety: fails if NeonSynthProcessor::processBlock allocates.

    Global operator new/delete are replaced with versions that count calls
    made while the calling thread is inside processBlock. The processor then
    renders a busy session - chords, the sequencer at 1/32, every waveform,
    every source and loop mode, parameter changes - while a second thread keeps replacing the sample
    (file loads, the default pad, embedded FLAC via state), as the GUI or a
    host would. Any allocation or free on the audio thread is a failure.

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target RealtimeSafetyTests
    Run the resulting RealtimeSafetyTests executable; it exits non-zero on failure.
*/
#include "NeonSynthProcessor.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

//==============================================================================
// Allocation hooks

namespace
{
    thread_local bool inAudioCallback = false;
    std::atomic<long> audioThreadAllocations { 0 };
    std::atomic<long> audioThreadFrees { 0 };

    void* allocate (std::size_t size)
    {
        if (inAudioCallback)
            ++audioThreadAllocations;
        if (void* p = std::malloc (size == 0 ? 1 : size))
            return p;
        throw std::bad_alloc();
    }

    void* allocateAligned (std::size_t size, std::align_val_t alignment)
    {
        if (inAudioCallback)
            ++audioThreadAllocations;
       #if defined (_MSC_VER)
        if (void* p = _aligned_malloc (size == 0 ? 1 : size, (std::size_t) alignment))
            return p;
       #else
        const auto a = (std::size_t) alignment;
        if (void* p = std::aligned_alloc (a, ((size == 0 ? 1 : size) + a - 1) / a * a))
            return p;
       #endif
        throw std::bad_alloc();
    }

    void release (void* p) noexcept
    {
        if (p != nullptr && inAudioCallback)
            ++audioThreadFrees;
        std::free (p);
    }

    void releaseAligned (void* p) noexcept
    {
        if (p != nullptr && inAudioCallback)
            ++audioThreadFrees;
       #if defined (_MSC_VER)
        _aligned_free (p);
       #else
        std::free (p);
       #endif
    }
}

void* operator new   (std::size_t size)                                   { return allocate (size); }
void* operator new[] (std::size_t size)                                   { return allocate (size); }
void* operator new   (std::size_t size, const std::nothrow_t&) noexcept   { try { return allocate (size); } catch (...) { return nullptr; } }
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept   { try { return allocate (size); } catch (...) { return nullptr; } }
void* operator new   (std::size_t size, std::align_val_t a)               { return allocateAligned (size, a); }
void* operator new[] (std::size_t size, std::align_val_t a)               { return allocateAligned (size, a); }
void  operator delete   (void* p) noexcept                                { release (p); }
void  operator delete[] (void* p) noexcept                                { release (p); }
void  operator delete   (void* p, std::size_t) noexcept                   { release (p); }
void  operator delete[] (void* p, std::size_t) noexcept                   { release (p); }
void  operator delete   (void* p, std::align_val_t) noexcept              { releaseAligned (p); }
void  operator delete[] (void* p, std::align_val_t) noexcept              { releaseAligned (p); }
void  operator delete   (void* p, std::size_t, std::align_val_t) noexcept { releaseAligned (p); }
void  operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { releaseAligned (p); }

//==============================================================================
namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    constexpr double testRate = 48000.0;
    constexpr int    testBlockSize  = 256;

    void setParameter (juce::AudioProcessor& p, const juce::String& id, float value)
    {
        for (auto* param : p.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param); ranged != nullptr && ranged->getParameterID() == id)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (value));
    }

    juce::File writeTestWav()
    {
        const auto file = juce::File::createTempFile (".wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (24));

        juce::AudioBuffer<float> b (1, 22050);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * i / 44100.0));
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        return file;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the processor's editor code needs the message manager

    // The hooks must see an allocation made on the "audio" thread, or a pass below would mean nothing.
    {
        inAudioCallback = true;
        auto probe = std::make_unique<int> (1);
        probe.reset();
        inAudioCallback = false;
        check (audioThreadAllocations.exchange (0) == 1 && audioThreadFrees.exchange (0) == 1,
               "the allocation hooks count an allocation and a free made inside the callback");
    }

    NeonSynthProcessor processor;
    setParameter (processor, "seqOn", 1.0f);
    setParameter (processor, "seqSync", 0.0f);
    setParameter (processor, "seqRate", 2.0f);      // 1/32
    setParameter (processor, "seqTempo", 200.0f);
    processor.setRateAndBufferSizeDetails (testRate, testBlockSize);
    processor.prepareToPlay (testRate, testBlockSize);

    // A state carrying an embedded sample, to apply from the other thread.
    juce::MemoryBlock stateWithSample;
    const auto wavFile = writeTestWav();
    {
        juce::String error;
        processor.loadSampleFile (wavFile, error);
        processor.getStateInformation (stateWithSample);
    }

    // The "GUI" thread: replaces the sample over and over while audio renders.
    std::atomic<bool> stop { false };
    std::atomic<int>  swaps { 0 };
    std::thread swapper ([&]
    {
        juce::String error;
        while (! stop.load())
        {
            switch (swaps.load() % 3)
            {
                case 0:  processor.loadSampleFile (wavFile, error); break;
                case 1:  processor.useDefaultSample(); break;
                default: processor.setStateInformation (stateWithSample.getData(), (int) stateWithSample.getSize()); break;
            }
            processor.setChopStep (swaps.load() % 16, (swaps.load() / 16) % 2 == 0);   // the editor toggling CHOP steps
            ++swaps;
        }
    });

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (4096);
    const int blocks = (int) (20.0 * testRate / testBlockSize);   // 20 s of audio

    for (int b = 0; b < blocks; ++b)
    {
        midi.clear();
        if (b % 40 == 0)
            for (int n = 0; n < 6; ++n)
                midi.addEvent (juce::MidiMessage::noteOn (1, 48 + 5 * n, (juce::uint8) 100), n * 17);
        if (b % 40 == 30)
            for (int n = 0; n < 6; ++n)
                midi.addEvent (juce::MidiMessage::noteOff (1, 48 + 5 * n), n * 11);
        if (b % 200 == 0)
            setParameter (processor, "waveform", (float) ((b / 200) % 4));   // Sine, Square, Triangle, Sawtooth
        if (b % 300 == 0)
            setParameter (processor, "source", (float) ((b / 300) % 4));     // Oscillator, Sample, Granular, Random
        if (b % 450 == 0)
            setParameter (processor, "smpLoop", (float) ((b / 450) % 3));    // One-shot, Loop, Loop until release
        if (b % 70 == 0)
            setParameter (processor, "chopOvertone", (float) ((b / 70) % 5) * 50.0f);   // 0, 50, 100, 150, 200 %
        if (b % 110 == 0)
            setParameter (processor, "chopComb", (float) (1 + (b / 110) % 32));

        // Noise, Fold and the grain knobs (S6): without these the allocation check never ran the
        // noise generator, the folder with its oversampling filters, or the grain settings
        // hand-over. The periods are different (and mostly prime), so the values meet in many
        // combinations during the 20 s.
        if (b % 90 == 0)
            setParameter (processor, "noiseLevel", (float) ((b / 90) % 5) * 25.0f);        // 0, 25 ... 100 %
        if (b % 170 == 0)
            setParameter (processor, "foldDrive", 1.0f + (float) ((b / 170) % 4) * 3.0f);  // 1, 4, 7, 10
        if (b % 130 == 0)
            setParameter (processor, "foldMix", (float) ((b / 130) % 5) * 25.0f);          // 0 skips the fold
        if (b % 50 == 0)
            setParameter (processor, "grPos", (float) ((b / 50) % 5) * 0.25f);
        if (b % 150 == 0)
            setParameter (processor, "grScan", (float) ((b / 150) % 5) - 2.0f);            // -2 ... 2 x the file's speed
        if (b % 95 == 0)
            setParameter (processor, "grSize", 5.0f + (float) ((b / 95) % 4) * 165.0f);    // 5 ... 500 ms
        if (b % 85 == 0)
            setParameter (processor, "grDensity", 1.0f + (float) ((b / 85) % 4) * 66.0f);  // 1 ... 199 per second
        if (b % 65 == 0)
            setParameter (processor, "grSpray", (float) ((b / 65) % 5) * 0.25f);
        if (b % 75 == 0)
            setParameter (processor, "grJitter", (float) ((b / 75) % 5) * 25.0f);          // cents

        inAudioCallback = true;
        processor.processBlock (buffer, midi);
        inAudioCallback = false;
    }

    stop = true;
    swapper.join();
    processor.releaseResources();
    wavFile.deleteFile();

    char what[200];
    std::snprintf (what, sizeof (what), "processBlock made no allocations (%ld) and no frees (%ld) over %d blocks, with %d sample swaps on another thread",
                   audioThreadAllocations.load(), audioThreadFrees.load(), blocks, swaps.load());
    check (audioThreadAllocations.load() == 0 && audioThreadFrees.load() == 0, what);
    check (swaps.load() > 10, "the other thread really did replace the sample while audio ran");

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
