// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include "SampleData.h"

/**
    @file SampleReader.h

    Plays one note from a SampleData: pitched by reading at a rate, with
    4-point cubic Hermite interpolation, optionally looped with a crossfade.

    Pitch: the read increment is 2^((note - root) / 12) x fileRate / hostRate.
    Notes played well above the root read one of the sample's band-limited
    copies instead (1/2 or 1/4 rate), chosen at note-on so the increment stays
    at or below about 1.19 (3 semitones above unity). Up to 3 semitones above
    the root the full-rate buffer is used, so slight transpositions keep all
    their top end; beyond that the copy's anti-alias filter has already
    removed what would fold back.

    Looping: in the last `crossfade` samples before the loop end, the output
    crossfades (equal power) from the material there into the material just
    after the loop start; at the loop end the read position jumps to
    loopStart + crossfade, where the crossfade landed. So the loop never
    clicks, wherever its points are, and needs nothing before loopStart.

    Loop modes:
      oneShot           plays to the end and finishes (the voice then fades out in 2 ms)
      loop              loops for as long as the note sounds, release tail included
      loopUntilRelease  loops while the key is held, then plays on to the end

    Holds a reference on the sample (SampleData::users) from begin() to end(),
    so the sample can't be freed under it. Audio thread only: no allocation,
    no locks.
*/
class SampleReader
{
public:
    enum LoopMode { oneShot = 0, loop = 1, loopUntilRelease = 2 };

    /** Per-note settings, from the sample parameters (all positions 0..1 of the sample length). */
    struct Settings
    {
        int   rootNote = 60;
        float start = 0.0f;
        int   loopMode = loop;
        float loopStart = 0.0f;
        float loopEnd = 1.0f;
        float crossfadeSeconds = 0.010f;
    };

    /** Increments above which a smaller copy is read (3 semitones above unity). */
    static constexpr double maxFullRateIncrement = 1.189207115;   // 2^(3/12)

    SampleReader() = default;
    SampleReader (const SampleReader&) = delete;
    SampleReader& operator= (const SampleReader&) = delete;
    ~SampleReader() = default;   // never touches the sample: at shutdown it may already be gone

    /** Starts a note. Returns false (and plays nothing) if there is no sample. */
    bool begin (const SampleData* newSample, int midiNote, double hostSampleRate, const Settings& settings) noexcept
    {
        end();
        if (newSample == nullptr || newSample->getLength() <= 0 || hostSampleRate <= 0.0)
            return false;

        sample = newSample;
        sample->users.fetch_add (1);

        const double ratio = std::pow (2.0, (midiNote - settings.rootNote) / 12.0)
                           * sample->getSampleRate() / hostSampleRate;

        level = 0;
        while (level < SampleData::numLevels - 1 && ratio / (double) (1 << level) > maxFullRateIncrement)
            ++level;

        data      = sample->getData (level);
        length    = sample->getLength (level);
        increment = ratio / (double) (1 << level);
        levelRate = sample->getSampleRate() / (double) (1 << level);

        // Loop region, at least 2 crossfades long (and at least 4 samples).
        const double len = (double) length;
        loopStartPos = juce::jlimit (0.0, len, (double) settings.loopStart * len);
        loopEndPos   = juce::jlimit (0.0, len, (double) settings.loopEnd   * len);
        if (loopEndPos - loopStartPos < 4.0)
        {
            loopEndPos   = juce::jmin (len, loopStartPos + 4.0);
            loopStartPos = juce::jmax (0.0, loopEndPos - 4.0);
        }
        crossfade = juce::jlimit (0.0, (loopEndPos - loopStartPos) / 2.0, (double) settings.crossfadeSeconds * levelRate);
        fadeStart = loopEndPos - crossfade;

        mode = (LoopMode) juce::jlimit (0, 2, settings.loopMode);
        looping = mode != oneShot && length >= 4;
        stopLoopingAtJump = false;

        position = juce::jlimit (0.0, len, (double) settings.start * len);
        if (looping && position >= loopEndPos)
            position = loopStartPos;   // a start past the loop would never reach it

        finished = false;
        return true;
    }

    /** Exchanges two readers' notes (and their sample references). */
    void swapWith (SampleReader& other) noexcept
    {
        std::swap (sample, other.sample);          std::swap (data, other.data);
        std::swap (length, other.length);          std::swap (level, other.level);
        std::swap (increment, other.increment);    std::swap (levelRate, other.levelRate);
        std::swap (position, other.position);      std::swap (loopStartPos, other.loopStartPos);
        std::swap (loopEndPos, other.loopEndPos);  std::swap (crossfade, other.crossfade);
        std::swap (fadeStart, other.fadeStart);    std::swap (mode, other.mode);
        std::swap (looping, other.looping);        std::swap (stopLoopingAtJump, other.stopLoopingAtJump);
        std::swap (finished, other.finished);
    }

    /** Ends the note and releases the sample. Safe to call when idle. */
    void end() noexcept
    {
        if (sample != nullptr)
            sample->users.fetch_sub (1);
        sample = nullptr;
        data = nullptr;
        finished = true;
    }

    /** The key was released: in Loop-until-release mode, stop looping and play on to the end. */
    void keyReleased() noexcept
    {
        if (mode != loopUntilRelease || ! looping)
            return;

        if (position < fadeStart)
            looping = false;
        else
            stopLoopingAtJump = true;   // mid-crossfade: finish it, then stop looping after the jump
    }

    bool isActive() const noexcept     { return sample != nullptr; }

    /** True once a non-looping note has played past the end of the sample. */
    bool hasFinished() const noexcept  { return finished; }

    /** Read position as a fraction of the sample, for display. */
    float getPosition() const noexcept { return length > 0 ? (float) (position / (double) length) : 0.0f; }

    int getLevel() const noexcept      { return level; }
    double getIncrement() const noexcept   { return increment; }

    float nextSample() noexcept
    {
        if (finished)
            return 0.0f;

        float out;

        if (looping && position >= fadeStart && crossfade > 0.0)
        {
            const double into = position - fadeStart;              // 0 .. crossfade
            const float  t    = (float) (into / crossfade);        // 0 .. 1
            const float  angle = t * juce::MathConstants<float>::halfPi;
            out = std::cos (angle) * read (position) + std::sin (angle) * read (loopStartPos + into);
        }
        else
        {
            out = read (position);
        }

        position += increment;

        if (looping && position >= loopEndPos)
        {
            position = loopStartPos + crossfade + (position - loopEndPos);
            if (stopLoopingAtJump)
                looping = false;
        }
        else if (position >= (double) length)
        {
            finished = true;
        }

        return out;
    }

private:
    /** Cubic Hermite (Catmull-Rom) at a fractional position; the padding makes the neighbours safe to read. */
    float read (double pos) const noexcept
    {
        const auto i = (int) pos;
        const float t = (float) (pos - (double) i);
        const float xm1 = data[i - 1], x0 = data[i], x1 = data[i + 1], x2 = data[i + 2];
        return x0 + 0.5f * t * (x1 - xm1 + t * (2.0f * xm1 - 5.0f * x0 + 4.0f * x1 - x2 + t * (3.0f * (x0 - x1) + x2 - xm1)));
    }

    const SampleData* sample = nullptr;
    const float* data = nullptr;
    int    length = 0, level = 0;
    double increment = 1.0, levelRate = 48000.0, position = 0.0;
    double loopStartPos = 0.0, loopEndPos = 0.0, crossfade = 0.0, fadeStart = 0.0;
    LoopMode mode = loop;
    bool   looping = false, stopLoopingAtJump = false, finished = true;
};
