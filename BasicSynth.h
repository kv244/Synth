#pragma once
#include <JuceHeader.h>

class BasicSynthSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote    (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

struct SimpleBiquad
{
    float a1 = 0.0f, a2 = 0.0f, b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

    void reset()
    {
        x1 = x2 = y1 = y2 = 0.0f;
    }

    void setLowPass (double sampleRate, float cutoffHz)
    {
        if (sampleRate <= 0.0) return;

        float theta = juce::MathConstants<float>::twoPi * cutoffHz / (float) sampleRate;
        float d = 1.0f / std::tan (theta * 0.5f);
        float d2 = d * d;
        float sqrt2 = std::sqrt (2.0f);
        float scale = 1.0f / (d2 + sqrt2 * d + 1.0f);

        b0 = scale;
        b1 = 2.0f * scale;
        b2 = scale;
        a1 = 2.0f * (1.0f - d2) * scale;
        a2 = (d2 - sqrt2 * d + 1.0f) * scale;
    }

    float process (float in)
    {
        float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = out;
        return out;
    }
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
        filterL.reset();
        filterR.reset();
        osc.reset();
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

            float leftSample = raw;
            float rightSample = raw;

            if (filterEnabled)
            {
                leftSample = filterL.process (leftSample);
                rightSample = filterR.process (rightSample);
            }

            tempBuffer.setSample (0, sample, leftSample);
            if (ch > 1) tempBuffer.setSample (1, sample, rightSample);
        }

        const int numChannelsToCopy = std::min (outputBuffer.getNumChannels(), tempBuffer.getNumChannels());
        for (int channel = 0; channel < numChannelsToCopy; ++channel)
            outputBuffer.addFrom (channel, startSample, tempBuffer, channel, 0, numSamples);
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock, int numChannels)
    {
        juce::dsp::ProcessSpec spec { sampleRate,
                                      (juce::uint32) samplesPerBlock,
                                      (juce::uint32) numChannels };
        osc.prepare (spec);
        lastSampleRate = sampleRate;
        lastCutoff     = -1.0f;  // force coefficient recalculation at new sample rate
        updateRates();
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
            case 2: osc.initialise ([](float x) {
                float x_scaled = x * (2.0f / juce::MathConstants<float>::pi);
                if (x < -juce::MathConstants<float>::halfPi)
                    return -2.0f - x_scaled;
                else if (x < juce::MathConstants<float>::halfPi)
                    return x_scaled;
                else
                    return 2.0f - x_scaled;
            }); break;
            case 3: osc.initialise ([](float x) { return x * (1.0f / juce::MathConstants<float>::pi); }); break;
            default: break;
        }
    }

    void setFilterEnabled (bool enabled) { filterEnabled = enabled; }

    void setFilterCutoff (float cutoffHz)
    {
        if (lastSampleRate > 0 && cutoffHz != lastCutoff)
        {
            lastCutoff = cutoffHz;
            filterL.setLowPass (lastSampleRate, cutoffHz);
            filterR.setLowPass (lastSampleRate, cutoffHz);
        }
    }

    void setADSR (float a, float d, float s, float r)
    {
        attackTime   = std::max (a, 0.001f);
        decayTime    = std::max (d, 0.001f);
        sustainLevel = juce::jlimit (0.0f, 1.0f, s);
        releaseTime  = std::max (r, 0.001f);
        updateRates();
    }

private:
    enum class EnvState { Idle, Attack, Decay, Sustain, Release };

    void updateRates()
    {
        if (lastSampleRate > 0.0)
        {
            attackRate  = 1.0f / (float) (lastSampleRate * attackTime);
            decayRate   = 1.0f / (float) (lastSampleRate * decayTime);
            releaseRate = 1.0f / (float) (lastSampleRate * releaseTime);
        }
    }

    void advanceEnvelope()
    {
        switch (envState)
        {
            case EnvState::Attack:
                envelopeValue += attackRate;
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
                envelopeValue -= (envelopeValue - sustainLevel) * decayRate;
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
                envelopeValue -= envelopeValue * releaseRate;
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
    SimpleBiquad                  filterL, filterR;
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

    float    attackRate  = 0.0f;
    float    decayRate   = 0.0f;
    float    releaseRate = 0.0f;
};
