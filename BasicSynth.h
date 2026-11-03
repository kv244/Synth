#pragma once
#include <JuceHeader.h>

class BasicSynthSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote    (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

/**
    One synth voice: band-limited oscillator -> ADSR -> TPT low-pass.

    Real-time safe: no allocation, no std::function, renders straight into the
    output buffer (no scratch buffer to overrun), and every parameter that can
    click is smoothed or latched at a phase wrap.
*/
class BasicSynthVoice : public juce::SynthesiserVoice
{
public:
    /** Parameter snapshot, read once per block on the audio thread. */
    struct Params
    {
        int   waveform = 0;            // 0 Sine, 1 Square, 2 Triangle, 3 Sawtooth
        float cutoffHz = 2000.0f;
        bool  filterOn = true;
        juce::ADSR::Parameters adsr;
    };

    // -12 dB per voice. Measured: a full-velocity 8-note square chord peaks at about
    // +5 dBFS (was +18), so the output gain parameter is still needed for dense chords.
    static constexpr float voiceGain = 0.25f;

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<BasicSynthSound*> (s) != nullptr;
    }

    void prepare (double newSampleRate, int maxBlockSize)
    {
        sampleRate = newSampleRate;
        adsr.setSampleRate (newSampleRate);
        adsr.reset();

        filter.prepare ({ newSampleRate, (juce::uint32) maxBlockSize, 1 });
        filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        filter.setResonance (1.0f / juce::MathConstants<float>::sqrt2);   // Butterworth Q

        cutoff.reset (newSampleRate, 0.02);       // 20 ms, multiplicative (even in octaves)
        cutoff.setCurrentAndTargetValue (clampCutoff (cutoff.getTargetValue()));
        filter.setCutoffFrequency (cutoff.getCurrentValue());

        filterMix.reset (newSampleRate, 0.01);    // 10 ms crossfade for the on/off toggle
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
    }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        // Set the pitch directly: no glide from the previous note.
        phaseInc = (float) (juce::MidiMessage::getMidiNoteInHertz (midiNote) / sampleRate);
        level    = velocity * voiceGain;

        if (hardStopping)
        {
            hardStopping = false;
            adsr.setParameters (adsrParams);
        }

        // juce::ADSR attacks from the current level, and the phase and filter state
        // are kept, so retriggering or stealing a sounding voice doesn't click.
        adsr.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
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

        for (int i = startSample; i < startSample + numSamples; ++i)
        {
            if (cutoff.isSmoothing())
                filter.setCutoffFrequency (cutoff.getNextValue());

            const float dry = nextOscillatorSample() * level * adsr.getNextSample();
            const float wet = filter.processSample (0, dry);   // always run: state stays warm
            const float out = dry + filterMix.getNextValue() * (wet - dry);

            left[i] += out;
            if (right != nullptr)
                right[i] += out;

            if (! adsr.isActive())
            {
                if (isVoiceActive())
                    clearCurrentNote();
                filter.reset();
                break;
            }
        }
    }

private:
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
    float  phase = 0.0f, phaseInc = 0.0f, level = 0.0f;
    int    waveform = 0, pendingWaveform = 0;
    bool   hardStopping = false;

    juce::ADSR adsr;
    juce::ADSR::Parameters adsrParams;
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoff { 2000.0f };
    juce::SmoothedValue<float> filterMix { 1.0f };
};
