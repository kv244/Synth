#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

/**
    @file VoiceEnvelope.h

    Linear ADSR envelope for one synth voice.

    It replaces juce::ADSR, which this synth outgrew in two ways:

      - Sustain changes jump. In its sustain stage juce::ADSR returns the new
        sustain value on the very next sample, so automating or turning the
        Sustain knob while a note is held makes an audible step. Here the
        sustain level slews to its target at most full scale per 10 ms.

      - Parameter updates during release break the release. The voice passes
        the current parameters every block; juce::ADSR then recomputes its
        release rate from the *sustain* level instead of the level the note was
        released at, so a note released during its attack or decay fades at
        the wrong speed, and with Sustain at 0 it stops dead (a click). Here
        the release rate is fixed when the note is released, from the level at
        that moment, and only recomputed (from the current level) if the
        Release time itself changes.

    Everything else matches juce::ADSR: linear segments whose lengths are the
    parameter times, attack from the current level (so retriggering never
    clicks), and the same Parameters struct.

    Audio thread only; no allocation, no locks.
*/
class VoiceEnvelope
{
public:
    using Parameters = juce::ADSR::Parameters;

    /** Time for the sustain level to slew across its full range. */
    static constexpr double sustainSlewSeconds = 0.010;

    VoiceEnvelope() noexcept   { recalculateRates(); }

    void setSampleRate (double newSampleRate) noexcept
    {
        sampleRate     = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        maxSustainStep = (float) (1.0 / (sustainSlewSeconds * sampleRate));
        recalculateRates();
    }

    /** Safe to call every block, in any stage. */
    void setParameters (const Parameters& newParameters) noexcept
    {
        const bool releaseTimeChanged = newParameters.release != parameters.release;
        parameters = newParameters;
        recalculateRates();

        if (state == State::release && releaseTimeChanged)
            startRelease();   // the remaining fade takes the new release time, from where it is now
    }

    const Parameters& getParameters() const noexcept   { return parameters; }

    void reset() noexcept
    {
        level        = 0.0f;
        sustainLevel = parameters.sustain;
        state        = State::idle;
    }

    /** Starts (or restarts) the attack from the current level. */
    void noteOn() noexcept
    {
        if (state == State::idle)
            sustainLevel = parameters.sustain;   // a silent voice has nothing to slew from

        if (attackRate > 0.0f)
        {
            state = State::attack;
        }
        else if (decayRate > 0.0f)
        {
            level = 1.0f;
            state = State::decay;
        }
        else
        {
            level = sustainLevel;
            state = State::sustain;
        }
    }

    /** Starts the release from the current level. */
    void noteOff() noexcept
    {
        if (state != State::idle)
            startRelease();
    }

    bool isActive() const noexcept   { return state != State::idle; }

    float getNextSample() noexcept
    {
        sustainLevel += juce::jlimit (-maxSustainStep, maxSustainStep, parameters.sustain - sustainLevel);

        switch (state)
        {
            case State::idle:
                return 0.0f;

            case State::attack:
                level = attackRate > 0.0f ? level + attackRate : 1.0f;
                if (level >= 1.0f)
                {
                    level = 1.0f;
                    state = decayRate > 0.0f ? State::decay : State::sustain;
                }
                break;

            case State::decay:
                level -= juce::jmax (decayRate, 0.0f);
                if (level <= sustainLevel || decayRate <= 0.0f)
                {
                    level = sustainLevel;
                    state = State::sustain;
                }
                break;

            case State::sustain:
                level = sustainLevel;
                break;

            case State::release:
                level -= releaseRate;
                if (level <= 0.0f)
                    reset();
                break;
        }

        return level;
    }

private:
    enum class State { idle, attack, decay, sustain, release };

    void recalculateRates() noexcept
    {
        const auto rate = [this] (float distance, float seconds)
        {
            return seconds > 0.0f ? (float) (distance / (seconds * sampleRate)) : -1.0f;
        };

        attackRate = rate (1.0f, parameters.attack);
        decayRate  = rate (1.0f - parameters.sustain, parameters.decay);
        // releaseRate is set by startRelease(), from the level at the time of release.
    }

    void startRelease() noexcept
    {
        if (parameters.release > 0.0f && level > 0.0f)
        {
            releaseRate = (float) (level / (parameters.release * sampleRate));
            state = State::release;
        }
        else
        {
            reset();
        }
    }

    Parameters parameters;
    State  state = State::idle;
    double sampleRate = 44100.0;
    float  level = 0.0f, sustainLevel = 1.0f;
    float  attackRate = 0.0f, decayRate = 0.0f, releaseRate = 0.0f;
    float  maxSustainStep = (float) (1.0 / (sustainSlewSeconds * 44100.0));
};
