#pragma once
#include <JuceHeader.h>

class BasicSynthSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote    (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

class BasicSynthVoice : public juce::SynthesiserVoice
{
public:
    BasicSynthVoice() = default;

    bool canPlaySound (juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<BasicSynthSound*> (sound) != nullptr;
    }

    void startNote (int midiNoteNumber, float velocity,
                    juce::SynthesiserSound*, int) override
    {
        envState      = EnvState::Attack;
        envelopeValue = 0.0f;
        filter.reset();
        osc.setFrequency (juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber));
        level = velocity;
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
            envState = EnvState::Release;
        else
        {
            clearCurrentNote();
            envelopeValue = 0.0f;
            envState      = EnvState::Idle;
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                          int startSample, int numSamples) override
    {
        if (envState == EnvState::Idle)
            return;

        const int ch = tempBuffer.getNumChannels();

        for (int sample = 0; sample < numSamples; ++sample)
        {
            advanceEnvelope();

            if (envState == EnvState::Idle)
            {
                // Note finished mid-block: zero remaining samples
                for (int s = sample; s < numSamples; ++s)
                {
                    tempBuffer.setSample (0, s, 0.0f);
                    if (ch > 1) tempBuffer.setSample (1, s, 0.0f);
                }
                break;
            }

            float raw = osc.processSample (0.0f) * level * envelopeValue;
            tempBuffer.setSample (0, sample, raw);
            if (ch > 1) tempBuffer.setSample (1, sample, raw);
        }

        if (filterEnabled)
        {
            juce::dsp::AudioBlock<float> block (tempBuffer);
            auto sub = block.getSubBlock (0, (size_t) numSamples);
            filter.process (juce::dsp::ProcessContextReplacing<float> (sub));
        }

        for (int channel = 0; channel < outputBuffer.getNumChannels(); ++channel)
            outputBuffer.addFrom (channel, startSample, tempBuffer, channel, 0, numSamples);
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock, int numChannels)
    {
        juce::dsp::ProcessSpec spec { sampleRate,
                                      (juce::uint32) samplesPerBlock,
                                      (juce::uint32) numChannels };
        osc.prepare (spec);
        filter.prepare (spec);
        lastSampleRate = sampleRate;
        lastCutoff     = -1.0f;  // force coefficient recalculation at new sample rate
        tempBuffer.setSize (numChannels, samplesPerBlock);
    }

    void setWaveform (int type)
    {
        if (type == currentWaveform) return;
        currentWaveform = type;
        switch (type)
        {
            case 0: osc.initialise ([](float x) { return std::sin (x); }); break;
            case 1: osc.initialise ([](float x) { return x < 0.0f ? -1.0f : 1.0f; }); break;
            case 2: osc.initialise ([](float x) { return std::asin (std::sin (x)) * (2.0f / juce::MathConstants<float>::pi); }); break;
            case 3: osc.initialise ([](float x) { return (2.0f / juce::MathConstants<float>::pi) * (x - juce::MathConstants<float>::pi); }); break;
            default: break;
        }
    }

    void setFilterEnabled (bool enabled) { filterEnabled = enabled; }

    void setFilterCutoff (float cutoffHz)
    {
        if (lastSampleRate > 0 && cutoffHz != lastCutoff)
        {
            lastCutoff = cutoffHz;
            filter.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (lastSampleRate, cutoffHz);
        }
    }

    void setADSR (float a, float d, float s, float r)
    {
        attackTime   = std::max (a, 0.001f);
        decayTime    = std::max (d, 0.001f);
        sustainLevel = juce::jlimit (0.0f, 1.0f, s);
        releaseTime  = std::max (r, 0.001f);
    }

private:
    enum class EnvState { Idle, Attack, Decay, Sustain, Release };

    void advanceEnvelope()
    {
        switch (envState)
        {
            case EnvState::Attack:
                envelopeValue += 1.0f / (float) (lastSampleRate * attackTime);
                if (envelopeValue >= 1.0f)
                {
                    envelopeValue = 1.0f;
                    envState = EnvState::Decay;
                }
                break;

            case EnvState::Decay:
                // Guard: if sustain was raised above current level, snap to Sustain
                if (envelopeValue <= sustainLevel)
                {
                    envelopeValue = sustainLevel;
                    envState = EnvState::Sustain;
                    break;
                }
                envelopeValue -= (envelopeValue - sustainLevel)
                                 / (float) (lastSampleRate * decayTime);
                if (envelopeValue <= sustainLevel + 0.0001f)
                {
                    envelopeValue = sustainLevel;
                    envState = EnvState::Sustain;
                }
                break;

            case EnvState::Sustain:
                envelopeValue = sustainLevel;
                break;

            case EnvState::Release:
                envelopeValue -= envelopeValue / (float) (lastSampleRate * releaseTime);
                if (envelopeValue < 0.0001f)
                {
                    clearCurrentNote();
                    envelopeValue = 0.0f;
                    envState = EnvState::Idle;
                }
                break;

            default: break;
        }
    }

    juce::dsp::Oscillator<float>  osc;
    juce::dsp::IIR::Filter<float> filter;
    juce::AudioBuffer<float>      tempBuffer;

    float  level          = 0.0f;
    float  envelopeValue  = 0.0f;
    bool   filterEnabled  = true;
    double lastSampleRate = 0.0;
    int    currentWaveform = -1;
    float  lastCutoff      = -1.0f;

    EnvState envState    = EnvState::Idle;
    float    attackTime  = 0.01f;
    float    decayTime   = 0.10f;
    float    sustainLevel = 0.7f;
    float    releaseTime = 0.30f;
};
