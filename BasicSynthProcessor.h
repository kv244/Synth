#pragma once
#include <JuceHeader.h>
#include "BasicSynth.h"
#include "MidiLogger.h"
#include "StepSequencer.h"

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
    // Output soft clipper (applied after the output gain). Below the knee the
    // signal passes through untouched; above it, it bends smoothly (tanh) towards
    // the ceiling and can never exceed it. Stateless, so no latency and no pumping.
    static constexpr float softClipKnee    = 0.50118723f;   // -6 dBFS
    static constexpr float softClipCeiling = 0.89125094f;   // -1 dBFS

    static float softClip (float x) noexcept
    {
        const float magnitude = std::abs (x);
        if (magnitude <= softClipKnee)
            return x;

        constexpr float range = softClipCeiling - softClipKnee;
        return std::copysign (softClipKnee + range * std::tanh ((magnitude - softClipKnee) / range), x);
    }

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
    // Step sequencer (see StepSequencer.h for the engine and its timing model)

    /** How the keyboard and the sequencer share the synth (the Sequencer Mode parameter). */
    enum class SequencerMode
    {
        layer,      ///< sequencer and keyboard both play
        replace,    ///< sequencer plays, keyboard notes are ignored
        transpose   ///< the pattern plays only while a key is held, shifted by (key - C4)
    };

    /** Reads a step of the pattern. Any thread. */
    StepSequencer::Step getSequencerStep (int index) const noexcept   { return sequencer.getStep (index); }

    /**
        Edits a step of the pattern. Message thread: besides the lock-free
        store, it tells the host the plugin state changed so the project is
        marked as modified.
    */
    void setSequencerStep (int index, const StepSequencer::Step& step);

    /** Step that most recently started, or -1 while the sequencer is stopped. Any thread. */
    int getSequencerPlayhead() const noexcept   { return sequencer.getCurrentStep(); }

    /** Tempo the sequencer followed in the last block (host or internal), for display. */
    float getSequencerTempo() const noexcept    { return sequencerTempo.load (std::memory_order_relaxed); }

    /** True when the last block was clocked by the host transport rather than the internal tempo. */
    bool isSequencerFollowingHost() const noexcept   { return sequencerFollowsHost.load (std::memory_order_relaxed); }

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

    /**
        Builds the MIDI the synth will play this block: keyboard input filtered
        by the sequencer mode, plus the sequencer's own events. Audio thread.
    */
    void buildPlayMidi (const juce::MidiBuffer& input, int numSamples) noexcept;

    /** Keeps the list of held keys up to date (Transpose mode). */
    void trackHeldKey (int note, bool isDown) noexcept;

    /** Fills the pattern with the default bassline (used until a session supplies one). */
    void loadDefaultPattern() noexcept;

    StepSequencer    sequencer;
    juce::MidiBuffer playMidi;                 // keyboard + sequencer events for the synth; capacity reserved in prepareToPlay
    double internalBeat   = 0.0;               // internal clock position in beats
    bool   wasSequencerOn = false;
    std::array<int, 16> heldKeys {};           // Transpose mode: keys currently down, most recent last
    int    numHeldKeys = 0;
    std::atomic<float> sequencerTempo { 120.0f };
    std::atomic<bool>  sequencerFollowsHost { false };

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

    juce::AudioParameterBool*   seqOnParameter         = nullptr;
    juce::AudioParameterBool*   seqSyncParameter       = nullptr;
    juce::AudioParameterFloat*  seqTempoParameter      = nullptr;
    juce::AudioParameterChoice* seqRateParameter       = nullptr;
    juce::AudioParameterFloat*  seqSwingParameter      = nullptr;
    juce::AudioParameterFloat*  seqGateParameter       = nullptr;
    juce::AudioParameterChoice* seqModeParameter       = nullptr;

    juce::SmoothedValue<float> outputGain { 1.0f };

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicSynthAudioProcessor)
};
