// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "NeonSynthVoice.h"
#include "MidiLogger.h"
#include "StepSequencer.h"
#include "SampleSlot.h"
#include "SampleLoader.h"

class NeonSynthProcessor : public juce::AudioProcessor
{
public:
    NeonSynthProcessor();
    ~NeonSynthProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    using AudioProcessor::processBlock;   // keep the double-precision overload visible
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "NeonSynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    /** Release time plus the 5 ms hard-stop fade (the envelope's release is linear and exact). */
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
    // signal passes through untouched; above it, it bends smoothly towards the
    // ceiling and can never exceed it. Stateless, so no latency and no pumping.
    //
    // The bend is a tanh-like rational curve, u (27 + u^2) / (27 + 9 u^2), which
    // has slope 1 at the knee, rises monotonically and meets the ceiling with
    // zero slope at u = 3 (an input of about +4.5 dBFS), staying there above.
    // It costs a few multiplies and one divide: about 4 ns per clipped sample
    // with MSVC, against about 16 ns for std::tanh.
    static constexpr float softClipKnee    = 0.50118723f;   // -6 dBFS
    static constexpr float softClipCeiling = 0.89125094f;   // -1 dBFS

    static float softClip (float x) noexcept
    {
        const float magnitude = std::abs (x);
        if (magnitude <= softClipKnee)
            return x;

        constexpr float range = softClipCeiling - softClipKnee;
        const float u    = juce::jmin ((magnitude - softClipKnee) / range, 3.0f);
        const float bend = u * (27.0f + u * u) / (27.0f + 9.0f * u * u);   // 0..1
        return x < 0.0f ? -(softClipKnee + range * bend) : softClipKnee + range * bend;
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
        std::atomic<float> samplePosition { -1.0f };   // 0..1 while playing the sample, else -1
        std::array<std::atomic<float>, NeonSynthVoice::Telemetry::maxShownGrains> grainPositions;   // 0..1 per sounding grain, else -1
        std::atomic<bool>  playingRandom { false };
        std::array<std::atomic<float>, RandomStepOscillator::numSteps> randomLevels;   // valid while playingRandom

        VoiceView()
        {
            for (auto& g : grainPositions) g.store (-1.0f);
            for (auto& l : randomLevels)   l.store (0.0f);
        }
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
    // Sample (played by the Sample and Granular sources, v1.5.0). One sample
    // per instance; a built-in pad until a file is loaded. See SampleData.h
    // for how a file is prepared and SampleSlot.h for the lock-free hand-off.

    /** What the editor shows about the sample. */
    struct SampleStatus
    {
        juce::String name;       ///< file name, or "Default pad"
        juce::String problem;    ///< why the last load failed, or "sample missing: <name>"; empty when fine
        bool loading = false;    ///< a background load is in progress
        double seconds = 0.0;
        double sampleRate = 0.0; ///< the file's own sample rate (playback converts to the host's)
        juce::String path;       ///< full path of the file; empty for the built-in pad
    };

    /**
        Message thread. Starts loading a WAV, AIFF or FLAC file in the
        background; the sample switches once it's ready. A newer request
        supersedes an older one that hasn't finished.
    */
    void loadSampleFileAsync (const juce::File& file);

    /** Any thread except the audio thread. Loads now; on failure the current sample stays and `error` says why. */
    bool loadSampleFile (const juce::File& file, juce::String& error);

    /** Any thread except the audio thread. Goes back to the built-in pad. */
    void useDefaultSample();

    /** Message thread (or any thread except the audio thread). */
    SampleStatus getSampleStatus() const;

    /** Calls `fn (const SampleData*)` with the current sample, safely against a concurrent load. Not for the audio thread. */
    template <typename Fn>
    auto withCurrentSample (Fn&& fn) const   { return sampleSlot.withCurrent (std::forward<Fn> (fn)); }

    //==========================================================================
    // CHOP's pattern (v1.6.0): 16 steps, saved with the session, not host parameters.

    /** Any thread except the audio thread. Sounding voices pick it up at their next cycle. */
    void setChopStep (int step, bool chopped) noexcept;

    /** Bit k set = step k (0..15) chopped. */
    juce::uint16 getChopPattern() const noexcept   { return chopPattern.load(); }

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
    std::vector<NeonSynthVoice*> voices;    // owned by synth; cached to avoid dynamic_cast per block
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

    juce::AudioParameterChoice* sourceParameter        = nullptr;
    juce::AudioParameterInt*    smpRootParameter       = nullptr;
    juce::AudioParameterFloat*  smpStartParameter      = nullptr;
    juce::AudioParameterChoice* smpLoopParameter       = nullptr;
    juce::AudioParameterFloat*  smpLoopStartParameter  = nullptr;
    juce::AudioParameterFloat*  smpLoopEndParameter    = nullptr;
    juce::AudioParameterFloat*  smpXfadeParameter      = nullptr;

    juce::AudioParameterFloat*  grPosParameter         = nullptr;
    juce::AudioParameterFloat*  grScanParameter        = nullptr;
    juce::AudioParameterFloat*  grSizeParameter        = nullptr;
    juce::AudioParameterFloat*  grDensityParameter     = nullptr;
    juce::AudioParameterFloat*  grSprayParameter       = nullptr;
    juce::AudioParameterFloat*  grJitterParameter      = nullptr;
    juce::AudioParameterFloat*  chopOvertoneParameter  = nullptr;
    juce::AudioParameterInt*    chopCombParameter      = nullptr;
    juce::AudioParameterFloat*  noiseLevelParameter    = nullptr;
    juce::AudioParameterFloat*  foldDriveParameter     = nullptr;
    juce::AudioParameterFloat*  foldMixParameter       = nullptr;

    /** CHOP's pattern, bit k = step k chopped. Written by the editor, read once per block; lock-free. */
    std::atomic<juce::uint16> chopPattern { ChopSettings().pattern };

    juce::SmoothedValue<float> outputGain { 1.0f };

    // Sample. sampleSlot must outlive sampleLoader (whose jobs publish into it), so it is declared first.
    SampleSlot sampleSlot;
    const SampleData* blockSample = nullptr;   // audio thread: the sample for the current block
    void publishSample (std::unique_ptr<SampleData> sample, const SampleData::Origin* missing = nullptr);
    void restoreSample (const juce::ValueTree& sampleTree);   // setStateInformation()

    mutable juce::CriticalSection sampleInfoLock;   // guards the three fields below; never taken by the audio thread
    SampleData::Origin missingSample;          // a session's sample that couldn't be found (kept so saving doesn't lose it)
    juce::String missingSampleHash;
    juce::String sampleProblem;

    std::atomic<int>  sampleLoadGeneration { 0 };
    std::atomic<bool> sampleLoading { false };
    juce::ThreadPool  sampleLoader { juce::ThreadPoolOptions{}.withThreadName ("NeonSynth sample loader").withNumberOfThreads (1) };

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeonSynthProcessor)
};
