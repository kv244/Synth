#pragma once
#include <JuceHeader.h>

class BasicSynthSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

class BasicSynthVoice : public juce::SynthesiserVoice
{
public:
    BasicSynthVoice()
    {
        filter.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (44100.0, 2000.0);
    }

    bool canPlaySound (juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<BasicSynthSound*> (sound) != nullptr;
    }

    void startNote (int midiNoteNumber, float velocity,
                    juce::SynthesiserSound*, int) override
    {
        releasing = false;          // cancel any in-progress release
        filter.reset();             // clear filter delay-line from previous note
        auto freq = juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber);
        osc.setFrequency (freq);
        level = velocity;
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
            releasing = true;
        else
        {
            clearCurrentNote();
            level = 0.0f;
            releasing = false;
        }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override
    {
        for (int sample = 0; sample < numSamples; ++sample)
        {
            if (releasing)
            {
                level -= level / (lastSampleRate * releaseSeconds);
                if (level < 0.0001f)
                {
                    clearCurrentNote();
                    level = 0.0f;
                    releasing = false;
                    // zero remaining samples so stale buffer data is not mixed out
                    for (int s = sample; s < numSamples; ++s)
                    {
                        tempBuffer.setSample (0, s, 0.0f);
                        if (tempBuffer.getNumChannels() > 1)
                            tempBuffer.setSample (1, s, 0.0f);
                    }
                    break;
                }
            }

            float raw = osc.processSample (0.0f) * level;
            tempBuffer.setSample (0, sample, raw);
            if (tempBuffer.getNumChannels() > 1)
                tempBuffer.setSample (1, sample, raw);
        }

        if (filterEnabled)
        {
            juce::dsp::AudioBlock<float> tempBlock (tempBuffer);
            auto subBlock = tempBlock.getSubBlock (0, (size_t) numSamples);
            juce::dsp::ProcessContextReplacing<float> tempContext (subBlock);
            filter.process (tempContext);
        }

        for (int channel = 0; channel < outputBuffer.getNumChannels(); ++channel)
            outputBuffer.addFrom (channel, startSample, tempBuffer, channel, 0, numSamples);
    }

    void prepareToPlay (double sampleRate, int samplesPerBlock, int numChannels)
    {
        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) numChannels };
        osc.prepare (spec);
        filter.prepare (spec);
        tempBuffer.setSize (numChannels, samplesPerBlock);
    }

    void setWaveform (int type)
    {
        if (type == currentWaveform)
            return;
        currentWaveform = type;
        switch (type)
        {
            case 0: osc.initialise ([](float x){ return std::sin (x); }); break;
            case 1: osc.initialise ([](float x){ return x < 0.0f ? -1.0f : 1.0f; }); break;
            case 2: osc.initialise ([](float x){ return std::asin (std::sin (x)) * (2.0f/juce::MathConstants<float>::pi); }); break;
            case 3: osc.initialise ([](float x){ return (2.0f/juce::MathConstants<float>::pi) * (x - juce::MathConstants<float>::pi); }); break;
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

    void setLastSampleRate (double sampleRate) { lastSampleRate = sampleRate; }

private:
    juce::dsp::Oscillator<float> osc;
    juce::dsp::IIR::Filter<float> filter;
    juce::AudioBuffer<float> tempBuffer;
    float level = 0.0f;
    bool filterEnabled = true;
    bool releasing = false;
    static constexpr float releaseSeconds = 0.3f;
    double lastSampleRate = 0.0;
    int currentWaveform = -1;
    float lastCutoff = -1.0f;
};
