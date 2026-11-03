#pragma once
#include <JuceHeader.h>
#include "BasicSynth.h"
#include "MidiLogger.h"

class BasicSynthAudioProcessor : public juce::AudioProcessor
{
public:
    BasicSynthAudioProcessor();
    ~BasicSynthAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    using AudioProcessor::processBlock;   // keep the double-precision overload visible
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "BasicSynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    /** Release time plus the 5 ms hard-stop fade; accurate now that juce::ADSR is linear. */
    double getTailLengthSeconds() const override { return (double) releaseParameter->get() + 0.005; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    static constexpr int numVoices = 16;

    //==========================================================================
    // Per-voice state for the visualisers. Written once per block on the audio
    // thread (relaxed atomic stores), read by the editor's timer. Fields may be
    // from neighbouring blocks, which is fine for display.
    struct VoiceView
    {
        std::atomic<bool>  active { false };
        std::atomic<float> level { 0.0f };
        std::atomic<float> frequencyHz { 0.0f };
        std::atomic<float> secondsSinceOn { 0.0f };
        std::atomic<float> secondsSinceOff { -1.0f };
    };

    const std::array<VoiceView, numVoices>& getVoiceViews() const noexcept { return voiceViews; }

    //==========================================================================
    // Scope data: audio thread writes, editor timer (message thread) reads.
    static constexpr int scopeFifoSize = 1 << 15;   // ~0.7 s at 48 kHz

    /** Message thread only. Copies up to maxFrames stereo frames; returns how many. */
    int drainScope (float* destLeft, float* destRight, int maxFrames) noexcept;

private:
    void pushScope (const float* left, const float* right, int numFrames) noexcept;

    // Stereo frames, interleaved L/R: index i of the FIFO lives at 2i and 2i + 1.
    juce::AbstractFifo scopeFifo { scopeFifoSize };
    std::array<float, 2 * scopeFifoSize> scopeRing {};

    MidiLogger  midiLogger;
    juce::int64 samplesProcessed = 0;

    juce::Synthesiser synth;
    std::vector<BasicSynthVoice*> voices;    // owned by synth; cached to avoid dynamic_cast per block
    std::array<VoiceView, numVoices> voiceViews;

    void publishVoiceViews() noexcept;

    juce::AudioProcessorValueTreeState apvts;

    juce::AudioParameterChoice* waveformParameter      = nullptr;
    juce::AudioParameterFloat*  cutoffParameter        = nullptr;
    juce::AudioParameterBool*   filterEnabledParameter = nullptr;
    juce::AudioParameterFloat*  attackParameter        = nullptr;
    juce::AudioParameterFloat*  decayParameter         = nullptr;
    juce::AudioParameterFloat*  sustainParameter       = nullptr;
    juce::AudioParameterFloat*  releaseParameter       = nullptr;
    juce::AudioParameterFloat*  gainParameter          = nullptr;
    juce::AudioParameterFloat*  widthParameter         = nullptr;

    juce::SmoothedValue<float> outputGain { 1.0f };

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicSynthAudioProcessor)
};
