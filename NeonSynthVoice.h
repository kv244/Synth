// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "VoiceEnvelope.h"

class NeonSynthSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote    (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

/**
    One synth voice: band-limited oscillator -> ADSR (VoiceEnvelope) -> TPT low-pass -> pan.

    Stereo: each voice is panned by its pitch (C4 in the centre, two octaves
    either side reaching the edges), scaled by the Width parameter. The pan law
    is constant-power, normalised so a centred voice keeps unity gain on both
    channels - at Width 0 the output is identical to the old mono voice.

    Real-time safe: no allocation, no std::function, renders straight into the
    output buffer (no scratch buffer to overrun), and every parameter that can
    click is smoothed or latched at a phase wrap.
*/
class NeonSynthVoice : public juce::SynthesiserVoice
{
public:
    /** Parameter snapshot, read once per block on the audio thread. */
    struct Params
    {
        int   waveform = 0;            // 0 Sine, 1 Square, 2 Triangle, 3 Sawtooth
        float cutoffHz = 2000.0f;
        bool  filterOn = true;
        float width = 0.0f;            // stereo width, 0 = mono .. 1 = full spread
        juce::ADSR::Parameters adsr;
    };

    /** What the editor's visualisers need to know about this voice. */
    struct Telemetry
    {
        bool  active = false;
        float level = 0.0f;              // current envelope value, 0..1
        float frequencyHz = 0.0f;
        float secondsSinceOn = 0.0f;
        float secondsSinceOff = -1.0f;   // -1 while the key is held
    };

    Telemetry getTelemetry() const noexcept
    {
        Telemetry t;
        t.active          = adsr.isActive();
        t.level           = t.active ? lastEnvelope : 0.0f;
        t.frequencyHz     = frequencyHz;
        t.secondsSinceOn  = (float) ((double) samplesSinceOn / sampleRate);
        t.secondsSinceOff = samplesSinceOff < 0 ? -1.0f : (float) ((double) samplesSinceOff / sampleRate);
        return t;
    }

    // -12 dB per voice. A full-velocity 8-note square chord would peak at about
    // +5 dBFS; the processor's output soft clipper keeps the output under -1 dBFS.
    static constexpr float voiceGain = 0.25f;

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<NeonSynthSound*> (s) != nullptr;
    }

    void prepare (double newSampleRate, int maxBlockSize)
    {
        sampleRate = newSampleRate;
        adsr.setSampleRate (newSampleRate);
        adsr.reset();

        filter.prepare ({ newSampleRate, (juce::uint32) maxBlockSize, 1 });
        filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        filter.setResonance (1.0f / juce::MathConstants<float>::sqrt2);   // Butterworth Q

        level.reset (newSampleRate, 0.002);       // 2 ms: a retriggered or stolen voice changes velocity without a click
        level.setCurrentAndTargetValue (level.getTargetValue());

        cutoff.reset (newSampleRate, 0.02);       // 20 ms, multiplicative (even in octaves)
        cutoff.setCurrentAndTargetValue (clampCutoff (cutoff.getTargetValue()));
        filter.setCutoffFrequency (cutoff.getCurrentValue());

        filterMix.reset (newSampleRate, 0.01);    // 10 ms crossfade for the on/off toggle

        pan.reset (newSampleRate, 0.02);          // 20 ms so Width changes don't zipper
        pan.setCurrentAndTargetValue (notePan * width);
        updatePanGains (pan.getCurrentValue());
    }

    /** Audio thread, once per block before rendering. */
    void setParameters (const Params& p) noexcept
    {
        adsrParams = p.adsr;
        if (! hardStopping)
            adsr.setParameters (adsrParams);

        pendingWaveform = p.waveform;                     // applied at the next phase wrap
        cutoff.setTargetValue (clampCutoff (p.cutoffHz));
        filterMix.setTargetValue (p.filterOn ? 1.0f : 0.0f);

        width = p.width;
        pan.setTargetValue (notePan * width);
    }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        // Set the pitch directly: no glide from the previous note.
        frequencyHz     = (float) juce::MidiMessage::getMidiNoteInHertz (midiNote);
        phaseInc        = (float) (frequencyHz / sampleRate);
        samplesSinceOn  = 0;
        samplesSinceOff = -1;

        // A sounding voice (retrigger or steal) glides to its new velocity and
        // pan. A silent voice starts exactly at the current settings: its
        // smoothers stopped advancing when it went quiet, so without the snap a
        // cutoff or filter-toggle change made while it was idle would sweep
        // audibly over the first 10-20 ms of the note.
        notePan = juce::jlimit (-1.0f, 1.0f, (float) (midiNote - 60) / 24.0f);
        const float newLevel = velocity * voiceGain;

        if (adsr.isActive())
        {
            level.setTargetValue (newLevel);
            pan.setTargetValue (notePan * width);
        }
        else
        {
            level.setCurrentAndTargetValue (newLevel);
            pan.setCurrentAndTargetValue (notePan * width);
            updatePanGains (pan.getCurrentValue());
            cutoff.setCurrentAndTargetValue (cutoff.getTargetValue());
            filter.setCutoffFrequency (cutoff.getCurrentValue());
            filterMix.setCurrentAndTargetValue (filterMix.getTargetValue());
        }

        if (hardStopping)
        {
            hardStopping = false;
            adsr.setParameters (adsrParams);
        }

        // The envelope attacks from its current level, and the phase and filter state
        // are kept, so retriggering or stealing a sounding voice doesn't click.
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        samplesSinceOff = 0;

        if (allowTailOff)
        {
            adsr.noteOff();
            return;
        }

        // Hard stop (voice stolen, all sound off): 5 ms fade instead of a cut.
        // juce::Synthesiser keeps calling renderNextBlock() on free voices, so the fade still plays.
        hardStopping = true;
        adsr.setParameters ({ 0.001f, 0.001f, 1.0f, 0.005f });
        adsr.noteOff();
        clearCurrentNote();
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    using SynthesiserVoice::renderNextBlock;   // keep the double-precision overload visible
    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples) override
    {
        if (! adsr.isActive())
            return;

        auto* left  = output.getWritePointer (0);
        auto* right = output.getNumChannels() > 1 ? output.getWritePointer (1) : nullptr;

        int rendered = 0;

        for (int i = startSample; i < startSample + numSamples; ++i, ++rendered)
        {
            if (cutoff.isSmoothing())
                filter.setCutoffFrequency (cutoff.getNextValue());

            lastEnvelope    = adsr.getNextSample();
            const float dry = nextOscillatorSample() * level.getNextValue() * lastEnvelope;
            const float wet = filter.processSample (0, dry);   // always run: state stays warm
            const float out = dry + filterMix.getNextValue() * (wet - dry);

            if (pan.isSmoothing())
                updatePanGains (pan.getNextValue());

            if (right != nullptr)
            {
                left[i]  += out * gainLeft;
                right[i] += out * gainRight;
            }
            else
            {
                left[i] += out;   // mono bus: no panning
            }

            if (! adsr.isActive())
            {
                if (isVoiceActive())
                    clearCurrentNote();
                filter.reset();
                lastEnvelope = 0.0f;
                break;
            }
        }

        samplesSinceOn += rendered;
        if (samplesSinceOff >= 0)
            samplesSinceOff += rendered;
    }

private:
    /** Constant-power pan law, scaled so the centre is unity gain on both sides. */
    void updatePanGains (float p) noexcept
    {
        const float angle = (p + 1.0f) * juce::MathConstants<float>::pi * 0.25f;   // 0 .. pi/2
        gainLeft  = juce::MathConstants<float>::sqrt2 * std::cos (angle);
        gainRight = juce::MathConstants<float>::sqrt2 * std::sin (angle);
    }

    float clampCutoff (float hz) const noexcept
    {
        return juce::jlimit (20.0f, 0.45f * (float) sampleRate, hz);   // stay well below Nyquist
    }

    /** PolyBLEP residual: removes most aliasing from a step discontinuity. */
    static float polyBlep (float t, float dt) noexcept
    {
        if (t < dt)
        {
            t /= dt;
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt)
        {
            t = (t - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

    float nextOscillatorSample() noexcept
    {
        const float t  = phase;
        const float dt = phaseInc;
        float value;

        switch (waveform)
        {
            case 1:  // Square
                value = (t < 0.5f ? 1.0f : -1.0f)
                      + polyBlep (t, dt)
                      - polyBlep (std::fmod (t + 0.5f, 1.0f), dt);
                break;
            case 2:  // Triangle (continuous, so naive is fine)
                value = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
                break;
            case 3:  // Sawtooth
                value = 2.0f * t - 1.0f - polyBlep (t, dt);
                break;
            default: // Sine
                value = std::sin (juce::MathConstants<float>::twoPi * t);
                break;
        }

        phase += dt;
        if (phase >= 1.0f)
        {
            phase -= 1.0f;
            waveform = pendingWaveform;   // switch shapes only at the cycle boundary
        }
        return value;
    }

    double sampleRate = 44100.0;
    float  phase = 0.0f, phaseInc = 0.0f;
    int    waveform = 0, pendingWaveform = 0;
    bool   hardStopping = false;

    // Telemetry for the GUI (written here on the audio thread, copied out by the processor).
    float       frequencyHz = 0.0f, lastEnvelope = 0.0f;
    juce::int64 samplesSinceOn = 0, samplesSinceOff = -1;

    VoiceEnvelope adsr;
    VoiceEnvelope::Parameters adsrParams;
    juce::SmoothedValue<float> level { 0.0f };   // velocity * voiceGain
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 2000.0f };
    juce::SmoothedValue<float> filterMix { 1.0f };

    // Stereo placement
    float notePan = 0.0f, width = 0.0f, gainLeft = 1.0f, gainRight = 1.0f;
    juce::SmoothedValue<float> pan { 0.0f };
};
