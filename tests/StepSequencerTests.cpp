// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Unit tests for StepSequencer (sample-accurate timing, gate, swing,
    transport stop / jump, tolerance of small transport drift, block-size
    independence, triggers disabled, retriggering and step packing).

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target StepSequencerTests
    Run the resulting StepSequencerTests executable; it exits non-zero on failure.
*/
#include <juce_audio_basics/juce_audio_basics.h>
#include "../StepSequencer.h"
#include <cstdio>
#include <vector>

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %s  %s\n", condition ? "PASS" : "FAIL", what);
        if (! condition)
            ++failures;
    }

    struct Event { juce::int64 sample; bool on; int note; int velocity; };

    constexpr double testSampleRate = 48000.0;
    constexpr double testBpm        = 120.0;                         // 24000 samples per beat
    constexpr double testSamplesPerBeat = testSampleRate * 60.0 / testBpm;

    /** Runs the sequencer over `totalSamples` with the given block sizes, collecting absolute event times. */
    std::vector<Event> run (StepSequencer& seq, juce::int64 totalSamples, const StepSequencer::Settings& settings,
                            const std::vector<int>& blockSizes = { 512 }, double startBeat = 0.0)
    {
        std::vector<Event> events;
        juce::MidiBuffer midi;
        midi.ensureSize (4096);

        juce::int64 position = 0;
        size_t blockIndex = 0;

        while (position < totalSamples)
        {
            const int n = (int) std::min<juce::int64> (blockSizes[blockIndex++ % blockSizes.size()], totalSamples - position);
            StepSequencer::Transport t { true, startBeat + (double) position / testSamplesPerBeat, testBpm };

            midi.clear();
            seq.process (midi, n, t, settings);

            for (const auto m : midi)
            {
                const auto msg = m.getMessage();
                events.push_back ({ position + m.samplePosition, msg.isNoteOn(), msg.getNoteNumber(), msg.getVelocity() });
            }
            position += n;
        }
        return events;
    }

    /** Prepares a sequencer with every step on, notes 60..75 (StepSequencer holds atomics, so it is not copyable). */
    void makeAllOn (StepSequencer& seq)
    {
        seq.prepare (testSampleRate);
        for (int i = 0; i < StepSequencer::numSteps; ++i)
            seq.setStep (i, { true, 60 + i, 100 });
    }

    std::vector<Event> onsOnly (const std::vector<Event>& events)
    {
        std::vector<Event> out;
        for (const auto& e : events)
            if (e.on) out.push_back (e);
        return out;
    }
}

int main()
{
    StepSequencer::Settings settings;   // 1/16, no swing, 50% gate

    std::printf ("Timing\n");
    {
        StepSequencer seq; makeAllOn (seq);
        const auto ons = onsOnly (run (seq, (juce::int64) (4 * testSamplesPerBeat), settings));   // one bar
        bool exact = ons.size() == 16;
        for (size_t i = 0; exact && i < ons.size(); ++i)
            exact = ons[i].sample == (juce::int64) i * 6000 && ons[i].note == 60 + (int) i;
        check (exact, "16 steps per bar at 1/16, each exactly 6000 samples apart, notes in order");
    }

    std::printf ("Gate\n");
    {
        StepSequencer seq; makeAllOn (seq);
        const auto events = run (seq, (juce::int64) (4 * testSamplesPerBeat), settings);
        bool ok = true;
        for (const auto& e : events)
            if (! e.on && e.note == 60) ok = (e.sample == 3000);
        check (ok, "50% gate: note-off lands half a step (3000 samples) after the note-on");

        int ons = 0, offs = 0;
        for (const auto& e : events) (e.on ? ons : offs)++;
        check (ons == 16 && offs == 16, "every note-on has a matching note-off within the bar");
    }

    std::printf ("Swing\n");
    {
        StepSequencer seq; makeAllOn (seq);
        auto swung = settings;
        swung.swing = 0.5f;   // odd steps delayed by 0.5 * half a step = 1500 samples
        const auto ons = onsOnly (run (seq, (juce::int64) (4 * testSamplesPerBeat), swung));
        bool ok = ons.size() == 16;
        for (size_t i = 0; ok && i < ons.size(); ++i)
            ok = ons[i].sample == (juce::int64) i * 6000 + ((i & 1) ? 1500 : 0);
        check (ok, "swing 50% delays only the odd steps, by 1500 samples");
    }

    std::printf ("Rates\n");
    {
        StepSequencer seq; makeAllOn (seq);
        auto eighths = settings;
        eighths.rate = StepSequencer::Rate::eighth;
        check (onsOnly (run (seq, (juce::int64) (4 * testSamplesPerBeat), eighths)).size() == 8, "1/8: 8 steps per bar");

        StepSequencer seq2; makeAllOn (seq2);
        auto triplets = settings;
        triplets.rate = StepSequencer::Rate::sixteenthTriplet;
        check (onsOnly (run (seq2, (juce::int64) (4 * testSamplesPerBeat), triplets)).size() == 24, "1/16T: 24 steps per bar");
    }

    std::printf ("Block-size independence\n");
    {
        StepSequencer a; makeAllOn (a);
        StepSequencer b; makeAllOn (b);
        const auto even   = run (a, (juce::int64) (8 * testSamplesPerBeat), settings, { 512 });
        const auto uneven = run (b, (juce::int64) (8 * testSamplesPerBeat), settings, { 37, 1000, 4096, 1, 777 });
        bool same = even.size() == uneven.size();
        for (size_t i = 0; same && i < even.size(); ++i)
            same = even[i].sample == uneven[i].sample && even[i].on == uneven[i].on && even[i].note == uneven[i].note;
        check (same, "identical events with 512-sample blocks and irregular blocks (37/1000/4096/1/777)");
    }

    std::printf ("Transport\n");
    {
        StepSequencer seq; makeAllOn (seq);
        juce::MidiBuffer midi;
        midi.ensureSize (1024);

        seq.process (midi, 512, { true, 0.0, testBpm }, settings);              // step 0 starts, note-off pending
        midi.clear();
        seq.process (midi, 512, { false, 0.0, testBpm }, settings);             // transport stops
        int offs = 0;
        for (const auto m : midi) if (m.getMessage().isNoteOff() && m.samplePosition == 0) ++offs;
        check (offs == 1 && seq.getCurrentStep() == -1, "stopping releases the sounding note at once and parks the playhead");

        midi.clear();
        seq.process (midi, 512, { true, 1.0, testBpm }, settings);              // restart mid-bar (beat 1 = step 4)
        midi.clear();
        seq.process (midi, 512, { true, 0.0, testBpm }, settings);              // host loops back to beat 0
        bool releasedBeforeRestart = false;
        for (const auto m : midi)
            if (m.getMessage().isNoteOff() && m.samplePosition == 0) releasedBeforeRestart = true;
        check (releasedBeforeRestart && seq.getCurrentStep() == 0, "a jump in time releases notes, then playback resumes at the new position");
    }

    std::printf ("Transport drift\n");
    {
        // Host tempo changes from 120 to 126 BPM halfway through block 20. The host's
        // beat position follows the real tempo; each block reports the tempo at its
        // start, so the sequencer's prediction is about 26 samples short once.
        // Legato gate (100%), so a note is always sounding when that happens.
        StepSequencer seq; makeAllOn (seq);
        auto legato = settings;
        legato.gate = 1.0f;
        juce::MidiBuffer midi;
        midi.ensureSize (4096);
        const int block = 1024;
        double hostBeat = 0.0, bpm = testBpm;
        int ons = 0, cutShort = 0;
        std::vector<juce::int64> onTimes;
        juce::int64 noteOnAt[128] = {};
        juce::int64 position = 0;

        for (int b = 0; b < 200; ++b, position += block)
        {
            midi.clear();
            seq.process (midi, block, { true, hostBeat, bpm }, legato);
            for (const auto m : midi)
            {
                const auto msg = m.getMessage();
                const auto at  = position + m.samplePosition;
                if (msg.isNoteOn()) { ++ons; onTimes.push_back (at); noteOnAt[msg.getNoteNumber()] = at; }
                else if (at - noteOnAt[msg.getNoteNumber()] < 5600)   // a full step is 5714-6000 samples
                    ++cutShort;
            }

            const double spb = testSampleRate * 60.0 / bpm;
            if (b == 20)   // tempo change at the block's midpoint
            {
                hostBeat += (block / 2) / spb;
                bpm = 126.0;
                hostBeat += (block / 2) / (testSampleRate * 60.0 / bpm);
            }
            else
            {
                hostBeat += block / spb;
            }
        }
        bool noDuplicates = true;
        for (size_t i = 1; i < onTimes.size(); ++i)
            noDuplicates = noDuplicates && onTimes[i] - onTimes[i - 1] > 4000;   // steps are ~5700-6000 samples apart
        check (cutShort == 0 && noDuplicates && ons > 30,
               "a tempo change inside a block is not mistaken for a jump: no notes cut, none doubled");

        // Beat position jittering by up to +/-1 sample every block (host rounding).
        StepSequencer jit; makeAllOn (jit);
        juce::Random random (1234);
        int jitOns = 0;
        std::vector<juce::int64> jitTimes;
        position = 0;
        for (int b = 0; b < 188; ++b, position += 512)   // 96256 samples: four beats (16 steps) plus a little
        {
            const double jitter = b == 0 ? 0.0 : (random.nextFloat() * 2.0 - 1.0) / testSamplesPerBeat;
            midi.clear();
            jit.process (midi, 512, { true, (double) position / testSamplesPerBeat + jitter, testBpm }, settings);
            for (const auto m : midi)
            {
                const auto msg = m.getMessage();
                const auto at  = position + m.samplePosition;
                if (msg.isNoteOn()) { ++jitOns; jitTimes.push_back (at); noteOnAt[msg.getNoteNumber()] = at; }
                else if (std::abs (at - noteOnAt[msg.getNoteNumber()] - 3000) > 2) ++cutShort;   // 50% gate
            }
        }
        bool onGrid = jitOns == 17;   // steps 0..16
        for (size_t i = 0; onGrid && i < jitTimes.size(); ++i)
            onGrid = std::abs (jitTimes[i] - (juce::int64) i * 6000) <= 2;
        check (onGrid && cutShort == 0,
               "+/-1 sample of beat jitter: every step fires once, within 2 samples of its slot, at full length");

        // A small backward drift right after a step must not fire that step again.
        StepSequencer back; makeAllOn (back);
        midi.clear();
        back.process (midi, 512, { true, 5490.0 / testSamplesPerBeat, testBpm }, settings);       // step 1 (6000) at offset 510
        midi.clear();
        back.process (midi, 512, { true, 5999.0 / testSamplesPerBeat, testBpm }, settings);       // 3 samples back: 6000 is in this block too
        int again = 0;
        for (const auto m : midi) if (m.getMessage().isNoteOn()) ++again;
        check (again == 0, "a 3-sample backward drift does not retrigger the step that just fired");
    }

    std::printf ("Triggers disabled\n");
    {
        StepSequencer seq; makeAllOn (seq);
        juce::MidiBuffer midi;
        midi.ensureSize (1024);
        seq.process (midi, 512, { true, 0.0, testBpm }, settings);              // note-on at 0, off due at 3000
        auto quiet = settings;
        quiet.triggersEnabled = false;
        int ons = 0, offs = 0;
        for (int block = 1; block < 24; ++block)
        {
            midi.clear();
            seq.process (midi, 512, { true, block * 512 / testSamplesPerBeat, testBpm }, quiet);
            for (const auto m : midi) (m.getMessage().isNoteOn() ? ons : offs)++;
        }
        check (ons == 0 && offs == 1, "with triggers disabled no new notes start, but the pending note-off still arrives");
    }

    std::printf ("Retrigger\n");
    {
        StepSequencer seq;
        seq.prepare (testSampleRate);
        for (int i = 0; i < StepSequencer::numSteps; ++i)
            seq.setStep (i, { true, 64, 90 });                              // same note on every step
        auto legato = settings;
        legato.gate = 1.0f;                                                 // note-off exactly at the next step
        const auto events = run (seq, 12000, legato);                       // two steps
        // Expect: on@0, then at 6000 an off followed by an on (off first so the voice is released)
        bool ok = events.size() >= 3 && events[0].on && events[0].sample == 0
               && ! events[1].on && events[1].sample == 6000
               && events[2].on && events[2].sample == 6000;
        check (ok, "retriggering a still-sounding note sends its note-off first, at the same sample");
    }

    std::printf ("Packing\n");
    {
        bool ok = true;
        for (int note : { 0, 60, 127 })
            for (int vel : { 1, 64, 127 })
                for (bool on : { false, true })
                    ok = ok && StepSequencer::unpack (StepSequencer::pack ({ on, note, vel })) == StepSequencer::Step { on, note, vel };
        check (ok, "pack/unpack round-trips every on/note/velocity combination tested");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
