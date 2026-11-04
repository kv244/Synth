#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

/**
    @file StepSequencer.h

    A 16-step, sample-accurate MIDI step sequencer.

    How it fits into the plugin
    ---------------------------
    The sequencer produces MIDI, not audio. Each block, the processor asks it
    to write note-on / note-off events into a MidiBuffer at exact sample
    offsets, and that buffer is then handed to juce::Synthesiser exactly like
    keyboard input. Voice allocation, envelopes, stereo and the visualisers all
    work on sequenced notes for free.

    Timing model
    ------------
    Time is measured in beats (quarter notes). The caller describes each block
    with a Transport: the beat position at the first sample, the tempo, and
    whether time is running. Step k starts at beat k * stepLength, where
    stepLength depends on the Rate (0.25 beats for 1/16). Odd-numbered steps
    are delayed by the swing amount. Any step whose start falls inside
    [blockStart, blockEnd) is triggered on the first whole sample at or after
    its exact position:

        offset = ceil ((stepBeat - blockStart) * samplesPerBeat).

    Because step positions are derived from the transport every block (never
    accumulated), the pattern stays locked to the host's bar grid, and rate
    or tempo changes take effect on the next step without drift.

    Transport continuity
    --------------------
    Hosts don't advance their beat position perfectly: a tempo change in the
    middle of a block, a tempo ramp with large buffers, or rounding in the
    host can move the next block's start by a few samples from where this
    block predicted. Drift up to jumpToleranceSeconds (2 ms) either way is
    treated as continuous playback: nothing is released, and the last step
    that fired is remembered so a small backward drift never fires it twice
    and a small forward drift never skips one (a step that would fall just
    before the block fires at sample 0 instead). Anything larger is a jump
    (a loop, a locate): held notes are released and timing restarts from the
    new position.

    Note-offs
    ---------
    Every note-on schedules a note-off `gate * stepLength` later. Those can
    land in a later block, so pending note-offs are kept in a small fixed
    queue and counted down block by block. When time stops or jumps (a host
    loop, a locate, the sequencer being switched off), every pending note is
    released immediately so nothing hangs.

    Thread safety
    -------------
    process() runs on the audio thread and never allocates or locks. The
    pattern is stored as one std::atomic<uint32_t> per step, so the GUI can
    edit steps (setStep) while the audio thread reads them (getStep). The
    current playhead position is published the same way for the editor.

    Notes are sent on MIDI channel 16 so they can never cancel a note of the
    same pitch played on the keyboard (juce::Synthesiser matches note-offs by
    channel as well as note number).
*/
class StepSequencer
{
public:
    static constexpr int numSteps    = 16;
    static constexpr int midiChannel = 16;

    /** Transport drift up to this much (either way) is not treated as a jump. */
    static constexpr double jumpToleranceSeconds = 0.002;

    //==========================================================================
    /** One step of the pattern. */
    struct Step
    {
        bool on       = false;   ///< whether the step plays
        int  note     = 60;      ///< MIDI note number, 0..127 (kept while the step is off)
        int  velocity = 100;     ///< MIDI velocity, 1..127

        bool operator== (const Step& other) const noexcept
        {
            return on == other.on && note == other.note && velocity == other.velocity;
        }
    };

    /** Step length choices, as offered by the Rate parameter (same order). */
    enum class Rate { eighth, sixteenth, thirtySecond, eighthTriplet, sixteenthTriplet };

    /** Length of one step in beats (quarter notes) for a given rate. */
    static double stepLengthInBeats (Rate rate) noexcept
    {
        switch (rate)
        {
            case Rate::eighth:           return 0.5;
            case Rate::thirtySecond:     return 0.125;
            case Rate::eighthTriplet:    return 1.0 / 3.0;
            case Rate::sixteenthTriplet: return 1.0 / 6.0;
            case Rate::sixteenth:
            default:                     return 0.25;
        }
    }

    /** Where we are in musical time at the first sample of a block. */
    struct Transport
    {
        bool   playing = false;   ///< false = stopped: release everything, trigger nothing
        double beat    = 0.0;     ///< position in quarter notes at sample 0 of the block
        double bpm     = 120.0;   ///< tempo, quarter notes per minute
    };

    /** Per-block playback settings, read from parameters by the processor. */
    struct Settings
    {
        Rate  rate      = Rate::sixteenth;
        float swing     = 0.0f;    ///< 0..1: delays odd steps by up to half a step (0.33 is a triplet feel)
        float gate      = 0.5f;    ///< 0.05..1: note length as a fraction of one step
        int   transpose = 0;       ///< semitones added to every step's note
        bool  triggersEnabled = true;  ///< false = keep time but start no new notes (Transpose mode, no key held)
    };

    //==========================================================================
    StepSequencer()
    {
        for (auto& s : steps)
            s.store (pack ({}), std::memory_order_relaxed);
    }

    /** Call from prepareToPlay(). Clears any pending notes without emitting them. */
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        numPending = 0;
        wasPlaying = false;
        lastFiredBeat = noStepFired;
        currentStep.store (-1, std::memory_order_relaxed);
    }

    //==========================================================================
    /** Pattern editing. Safe to call from any thread (lock-free). */
    void setStep (int index, const Step& step) noexcept
    {
        if (juce::isPositiveAndBelow (index, numSteps))
            steps[(size_t) index].store (pack (step), std::memory_order_relaxed);
    }

    /** Returns a step of the pattern. Safe to call from any thread. */
    Step getStep (int index) const noexcept
    {
        return juce::isPositiveAndBelow (index, numSteps)
                 ? unpack (steps[(size_t) index].load (std::memory_order_relaxed))
                 : Step {};
    }

    /** Index of the step that most recently started, or -1 while stopped. For the GUI playhead. */
    int getCurrentStep() const noexcept   { return currentStep.load (std::memory_order_relaxed); }

    //==========================================================================
    /**
        Writes this block's sequencer events into `midi` (appending to what is
        already there). Audio thread; does not allocate as long as `midi` has
        been given enough capacity with MidiBuffer::ensureSize().

        @param midi        destination buffer, events are added at sample offsets 0..numSamples-1
        @param numSamples  length of the block
        @param transport   musical time at the start of the block
        @param settings    rate, swing, gate, transpose for this block
    */
    void process (juce::MidiBuffer& midi, int numSamples,
                  const Transport& transport, const Settings& settings) noexcept
    {
        if (numSamples <= 0)
            return;

        // Stopped: release whatever is sounding and park the playhead.
        if (! transport.playing || transport.bpm <= 0.0)
        {
            releaseAll (midi, 0);
            wasPlaying = false;
            lastFiredBeat = noStepFired;
            currentStep.store (-1, std::memory_order_relaxed);
            return;
        }

        const double samplesPerBeat = sampleRate * 60.0 / transport.bpm;
        const double blockStart     = transport.beat;
        const double blockEnd       = blockStart + (double) numSamples / samplesPerBeat;

        // Continuous playback, or a jump in time? Small drift is tolerated (see
        // "Transport continuity" above); a real jump (host loop, locate) releases
        // held notes so nothing hangs across the discontinuity. Just starting is
        // neither: there is nothing to release and no step has fired yet.
        const double toleranceSamples = jumpToleranceSeconds * sampleRate;
        const bool   continuous = wasPlaying
                               && std::abs (blockStart - expectedBeat) * samplesPerBeat <= toleranceSamples;

        if (! continuous)
        {
            if (wasPlaying)
                releaseAll (midi, 0);
            lastFiredBeat = noStepFired;
        }

        wasPlaying   = true;
        expectedBeat = blockEnd;

        const double stepLength  = stepLengthInBeats (settings.rate);
        const double swingDelay  = (double) juce::jlimit (0.0f, 1.0f, settings.swing) * 0.5 * stepLength;
        const int    gateSamples = juce::jmax (1, (int) std::lround ((double) juce::jlimit (0.05f, 1.0f, settings.gate)
                                                                     * stepLength * samplesPerBeat));

        // Candidate steps: start early enough to catch a swung odd step, or a step
        // skipped by forward drift, from just before the block; stop once a step
        // starts after the block.
        //
        // A step fires on the first whole sample at or after its exact position:
        // offset = ceil (distance in samples). Deciding on integer offsets (rather
        // than comparing beats) means every step lands in exactly one block and on
        // the same absolute sample whatever the block sizes are; the small epsilon
        // absorbs floating-point error such as 5999.9999999 for 6000.
        constexpr double epsilon = 1.0e-6;

        const double firstCandidate = blockStart - (continuous ? toleranceSamples / samplesPerBeat : 0.0);

        for (auto k = (juce::int64) std::floor (firstCandidate / stepLength) - 1; ; ++k)
        {
            const bool   odd      = (k & 1) != 0;
            const double stepBeat = (double) k * stepLength + (odd ? swingDelay : 0.0);
            const double distance = (stepBeat - blockStart) * samplesPerBeat;   // in samples, may be fractional
            auto         offset   = (juce::int64) std::ceil (distance - epsilon);

            if (offset >= numSamples)
                break;

            if (continuous)
            {
                // Already fired (it can reappear after a small backward drift)?
                if (stepBeat <= lastFiredBeat + epsilon / samplesPerBeat)
                    continue;
                // Skipped by a small forward drift: fire it now rather than never.
                if (offset < 0)
                {
                    if ((double) -offset > toleranceSamples)
                        continue;
                    offset = 0;
                }
            }
            else if (offset < 0)
            {
                continue;   // started (or jumped) after this step: don't play it late
            }

            lastFiredBeat = stepBeat;

            const int index = (int) (((k % numSteps) + numSteps) % numSteps);
            currentStep.store (index, std::memory_order_relaxed);

            const auto step = getStep (index);
            const int  note = step.note + settings.transpose;

            if (step.on && settings.triggersEnabled && juce::isPositiveAndBelow (note, 128))
                startNote (midi, note, step.velocity, (int) offset, gateSamples);
        }

        advancePendingNoteOffs (midi, numSamples);
    }

    /** Releases every sounding sequencer note at the given offset (e.g. when switched off). */
    void releaseAll (juce::MidiBuffer& midi, int offset) noexcept
    {
        for (int i = 0; i < numPending; ++i)
            midi.addEvent (juce::MidiMessage::noteOff (midiChannel, pending[(size_t) i].note), offset);
        numPending = 0;
    }

    //==========================================================================
    /** Packs a step into 32 bits: bit 16 = on, bits 8-15 = velocity, bits 0-7 = note. */
    static juce::uint32 pack (const Step& s) noexcept
    {
        return (s.on ? 0x10000u : 0u)
             | ((juce::uint32) juce::jlimit (1, 127, s.velocity) << 8)
             |  (juce::uint32) juce::jlimit (0, 127, s.note);
    }

    /** Inverse of pack(). */
    static Step unpack (juce::uint32 bits) noexcept
    {
        Step s;
        s.on       = (bits & 0x10000u) != 0;
        s.velocity = (int) ((bits >> 8) & 0x7fu);
        s.note     = (int) (bits & 0x7fu);
        return s;
    }

private:
    /** A note-off that is due `samplesLeft` samples after the start of the current block. */
    struct PendingNoteOff
    {
        int note = 0;
        int samplesLeft = 0;
    };

    /** Emits a note-on, first releasing the same note if it is still sounding. */
    void startNote (juce::MidiBuffer& midi, int note, int velocity, int offset, int gateSamples) noexcept
    {
        for (int i = 0; i < numPending; ++i)
        {
            if (pending[(size_t) i].note == note)
            {
                midi.addEvent (juce::MidiMessage::noteOff (midiChannel, note),
                               juce::jmin (offset, pending[(size_t) i].samplesLeft));
                removePending (i);
                break;
            }
        }

        midi.addEvent (juce::MidiMessage::noteOn (midiChannel, note, (juce::uint8) juce::jlimit (1, 127, velocity)), offset);

        if (numPending == (int) pending.size())   // queue full: release the oldest note now
        {
            midi.addEvent (juce::MidiMessage::noteOff (midiChannel, pending[0].note), offset);
            removePending (0);
        }
        pending[(size_t) numPending++] = { note, offset + gateSamples };
    }

    /** Emits the note-offs that fall inside this block and counts the rest down. */
    void advancePendingNoteOffs (juce::MidiBuffer& midi, int numSamples) noexcept
    {
        for (int i = 0; i < numPending;)
        {
            auto& p = pending[(size_t) i];
            if (p.samplesLeft < numSamples)
            {
                midi.addEvent (juce::MidiMessage::noteOff (midiChannel, p.note), p.samplesLeft);
                removePending (i);
            }
            else
            {
                p.samplesLeft -= numSamples;
                ++i;
            }
        }
    }

    void removePending (int i) noexcept
    {
        pending[(size_t) i] = pending[(size_t) (numPending - 1)];
        --numPending;
    }

    std::array<std::atomic<juce::uint32>, numSteps> steps;
    std::atomic<int> currentStep { -1 };

    std::array<PendingNoteOff, 32> pending {};
    int    numPending   = 0;
    double sampleRate   = 44100.0;
    double expectedBeat = 0.0;
    bool   wasPlaying   = false;

    static constexpr double noStepFired = -1.0e300;
    double lastFiredBeat = noStepFired;   ///< beat of the most recent step that fired, while playing continuously
};
