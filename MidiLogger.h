// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>

/**
    Real-time-safe MIDI logger.

    The audio thread copies each MIDI event into a fixed-size record in a
    pre-allocated lock-free FIFO (no allocation, no locks, no I/O). A low-priority
    background thread formats the records and writes them to disk.
    Timestamps come from a sample counter, so offline renders log audio time,
    not wall-clock time.
*/
class MidiLogger : private juce::Thread
{
public:
    MidiLogger() : juce::Thread ("NeonSynth MIDI log") {}
    ~MidiLogger() override { stop(); }

    /** Message thread. Truncates the file and starts the writer thread. */
    bool start (const juce::File& file)
    {
        stop();
        file.deleteFile();
        stream = file.createOutputStream();

        if (stream == nullptr || ! stream->openedOk())
        {
            stream.reset();
            return false;
        }

        enabled.store (true);
        return startThread (juce::Thread::Priority::low);
    }

    /** Message thread. Flushes anything pending and closes the file. */
    void stop()
    {
        enabled.store (false);
        stopThread (1000);
        drain();
        stream.reset();
    }

    /** Called from prepareToPlay(). */
    void setSampleRate (double newRate) noexcept    { sampleRate.store (newRate); }

    /** Audio thread: lock-free and allocation-free. Drops events if the FIFO is full. */
    void push (const juce::MidiBuffer& midi, juce::int64 blockStartSample) noexcept
    {
        if (! enabled.load (std::memory_order_relaxed))
            return;

        for (const auto metadata : midi)
        {
            Record r;
            r.samplePos = blockStartSample + metadata.samplePosition;
            r.numBytes  = (juce::uint8) juce::jmin (metadata.numBytes, (int) r.bytes.size());
            r.truncated = metadata.numBytes > (int) r.bytes.size();
            std::memcpy (r.bytes.data(), metadata.data, r.numBytes);

            auto scope = fifo.write (1);
            if (scope.blockSize1 + scope.blockSize2 == 0)
                dropped.fetch_add (1, std::memory_order_relaxed);
            else
                scope.forEach ([&] (int index) { records[(size_t) index] = r; });
        }
    }

private:
    struct Record
    {
        juce::int64 samplePos = 0;
        std::array<juce::uint8, 16> bytes {};
        juce::uint8 numBytes = 0;
        bool truncated = false;
    };

    void run() override
    {
        while (! threadShouldExit())
        {
            drain();
            wait (50);
        }
    }

    /** Writer side: the background thread, or the message thread after stopThread(). */
    void drain()
    {
        if (stream == nullptr)
            return;

        const auto rate = juce::jmax (1.0, sampleRate.load());
        bool wroteAnything = false;

        fifo.read (fifo.getNumReady()).forEach ([&] (int index)
        {
            const auto& r = records[(size_t) index];
            const auto hex = juce::String::toHexString (r.bytes.data(), (int) r.numBytes);

            juce::String desc;
            if (r.truncated)
                desc = "SysEx (truncated): " + hex;
            else if (r.numBytes > 0)
                desc = juce::MidiMessage (r.bytes.data(), (int) r.numBytes).getDescription();

            if (desc.isEmpty())
                desc = "Raw Bytes: " + hex;

            const auto line = "[" + juce::String::formatted ("%.3f", (double) r.samplePos / rate) + "s] " + desc + "\n";
            stream->write (line.toRawUTF8(), line.getNumBytesAsUTF8());
            wroteAnything = true;
        });

        if (const auto n = dropped.exchange (0); n > 0)
        {
            const auto line = "[dropped " + juce::String (n) + " events: log FIFO full]\n";
            stream->write (line.toRawUTF8(), line.getNumBytesAsUTF8());
            wroteAnything = true;
        }

        if (wroteAnything)
            stream->flush();
    }

    static constexpr int capacity = 4096;
    juce::AbstractFifo fifo { capacity };
    std::array<Record, capacity> records {};

    std::unique_ptr<juce::FileOutputStream> stream;
    std::atomic<bool>   enabled { false };
    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<int>    dropped { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiLogger)
};
