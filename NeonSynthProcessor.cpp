// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#include "NeonSynthProcessor.h"
#include "NeonSynthEditor.h"

namespace
{
    /**
        Log path from NEONSYNTH_MIDILOG, or (Standalone only) from "--midilog <file>".
        BASICSYNTH_MIDILOG, the name before v1.4, still works.
    */
    juce::String findMidiLogPath (juce::AudioProcessor::WrapperType wrapper)
    {
        auto path = juce::SystemStats::getEnvironmentVariable ("NEONSYNTH_MIDILOG", {});
        if (path.isEmpty())
            path = juce::SystemStats::getEnvironmentVariable ("BASICSYNTH_MIDILOG", {});

        if (path.isEmpty() && wrapper == juce::AudioProcessor::wrapperType_Standalone)
        {
            const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
            const auto index = args.indexOf ("--midilog");

            if (index >= 0 && index + 1 < args.size())
                path = args[index + 1].unquoted();
        }

        return path;
    }

    /**
        Each plugin instance that logs gets its own file, so instances never
        delete or overwrite each other's log: the first uses the path as given
        (relative to the working directory), later ones in the same process add
        "-2", "-3", ... before the extension (midi.log, midi-2.log, ...).
    */
    juce::File midiLogFileForNewInstance (const juce::String& path)
    {
        static std::atomic<int> loggingInstances { 0 };
        const int instance = ++loggingInstances;

        auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
        if (instance > 1)
            file = file.getSiblingFile (file.getFileNameWithoutExtension() + "-" + juce::String (instance)
                                        + file.getFileExtension());
        return file;
    }

    template <typename ParamType>
    ParamType* getTypedParameter (juce::AudioProcessorValueTreeState& state, const char* id)
    {
        auto* p = dynamic_cast<ParamType*> (state.getParameter (id));
        jassert (p != nullptr);   // wrong ID or wrong type
        return p;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout NeonSynthProcessor::createParameterLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    // Choice instead of Int: hosts show names. The stored value is still 0-3, so sessions load unchanged.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "waveform", 1 }, "Waveform",
                                                         StringArray { "Sine", "Square", "Triangle", "Sawtooth" }, 0));

    NormalisableRange<float> cutoffRange (20.0f, 20000.0f);
    cutoffRange.setSkewForCentre (1000.0f);   // half the knob travel below 1 kHz
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "cutoff", 1 }, "Cutoff", cutoffRange, 2000.0f));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "filterEnabled", 1 }, "Filter Enabled", true));

    // No unit labels: pedalboard (test_midi.py) appends them to parameter names ("cutoff_hz").
    const auto timeRange = [] (float max) { return NormalisableRange<float> (0.001f, max, 0.001f, 0.3f); };

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "attack",  1 }, "Attack",  timeRange (5.0f),  0.01f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "decay",   1 }, "Decay",   timeRange (5.0f),  0.10f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "sustain", 1 }, "Sustain", 0.0f, 1.0f, 0.7f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "release", 1 }, "Release", timeRange (10.0f), 0.30f));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain", 1 }, "Output Gain (dB)",
                                                        NormalisableRange<float> (-48.0f, 6.0f, 0.1f), 0.0f));

    // Stereo spread of voices by pitch: 0 = mono, 1 = notes two octaves from middle C hard-panned.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "width", 1 }, "Stereo Width", 0.0f, 1.0f, 0.6f));

    // Step sequencer. Off by default so the plugin behaves exactly as before until enabled.
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "seqOn",   1 }, "Sequencer On",   false));
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "seqSync", 1 }, "Sequencer Sync", true));    // follow the host transport when it has one
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "seqTempo", 1 }, "Sequencer Tempo",
                                                         NormalisableRange<float> (40.0f, 240.0f, 0.1f), 120.0f));   // BPM, internal clock
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "seqRate", 1 }, "Sequencer Rate",
                                                         StringArray { "1/8", "1/16", "1/32", "1/8T", "1/16T" }, 1));  // order matches StepSequencer::Rate
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "seqSwing", 1 }, "Sequencer Swing", 0.0f, 1.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "seqGate",  1 }, "Sequencer Gate",
                                                         NormalisableRange<float> (0.05f, 1.0f), 0.5f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "seqMode", 1 }, "Sequencer Mode",
                                                         StringArray { "Layer", "Replace", "Transpose" }, 0));        // order matches SequencerMode

    // Sound source (v1.5.0). A separate parameter rather than more Waveform choices: adding
    // items to Waveform would move its existing choices' normalised values and break
    // automation recorded with v1.4. New parameters go at the end so old sessions map cleanly.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "source", 1 }, "Source",
                                                         StringArray { "Oscillator", "Sample", "Granular", "Random" }, 0));   // order matches NeonSynthVoice::Source

    // Sample playback; positions are fractions of the sample's length.
    layout.add (std::make_unique<AudioParameterInt>    (ParameterID { "smpRoot", 1 }, "Sample Root", 0, 127, 60));   // MIDI note the file plays at its own pitch
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "smpStart", 1 }, "Sample Start", 0.0f, 1.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "smpLoop", 1 }, "Sample Loop",
                                                         StringArray { "One-shot", "Loop", "Loop until release" }, 1));  // order matches SampleReader::LoopMode
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "smpLoopStart", 1 }, "Sample Loop Start", 0.0f, 1.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "smpLoopEnd", 1 }, "Sample Loop End", 0.0f, 1.0f, 1.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "smpXfade", 1 }, "Sample Crossfade",
                                                         NormalisableRange<float> (5.0f, 50.0f, 0.1f), 10.0f));       // milliseconds

    // Granular. Position stays live; the others shape each grain as it starts.
    NormalisableRange<float> grainSizeRange (5.0f, 500.0f, 0.1f);
    grainSizeRange.setSkewForCentre (80.0f);
    NormalisableRange<float> grainDensityRange (1.0f, 200.0f, 0.1f);
    grainDensityRange.setSkewForCentre (30.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grPos", 1 }, "Grain Position", 0.0f, 1.0f, 0.5f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grScan", 1 }, "Grain Scan",
                                                        NormalisableRange<float> (-2.0f, 2.0f, 0.01f), 0.0f));          // x the file's speed; 0 freezes
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grSize", 1 }, "Grain Size", grainSizeRange, 80.0f));            // ms
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grDensity", 1 }, "Grain Density", grainDensityRange, 30.0f));   // grains per second
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grSpray", 1 }, "Grain Spray", 0.0f, 1.0f, 0.1f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "grJitter", 1 }, "Grain Pitch Jitter",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));          // cents

    // Waveform sculpting (v1.6.0), for the Oscillator and Random sources. Each default leaves the
    // sound as it was, so sessions saved before v1.6.0 render the same.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "chopOvertone", 1 }, "Chop Overtone",
                                                        NormalisableRange<float> (0.0f, 200.0f, 0.1f), 0.0f));          // %: chopped steps x (1 - Overtone/100)
    layout.add (std::make_unique<AudioParameterInt>   (ParameterID { "chopComb", 1 }, "Chop Comb", 1, 32, 1));      // pattern repeats per cycle
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "noiseLevel", 1 }, "Noise",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));          // %: crossfade to white noise
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "foldDrive", 1 }, "Fold Drive",
                                                        NormalisableRange<float> (1.0f, 10.0f, 0.01f), 3.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "foldMix", 1 }, "Fold Mix",
                                                        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));          // %: 0 skips the fold
    return layout;
}

NeonSynthProcessor::NeonSynthProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    for (int i = 0; i < numVoices; ++i)
    {
        voices.push_back (static_cast<NeonSynthVoice*> (synth.addVoice (new NeonSynthVoice())));
        voices.back()->setVoiceIndex (i);
    }

    synth.addSound (new NeonSynthSound());

    // By default juce::Synthesiser handles any MIDI event that falls within 32
    // samples of its last render point immediately, so a note could start up to
    // 31 samples early. Splitting on every event keeps keyboard and sequencer
    // notes on their exact sample; the extra sub-blocks only appear when events
    // are dense, and cost little (see docs/DEVELOPMENT.md, "Performance").
    synth.setMinimumRenderingSubdivisionSize (1, true);

    waveformParameter      = getTypedParameter<juce::AudioParameterChoice> (apvts, "waveform");
    cutoffParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "cutoff");
    filterEnabledParameter = getTypedParameter<juce::AudioParameterBool>   (apvts, "filterEnabled");
    attackParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "attack");
    decayParameter         = getTypedParameter<juce::AudioParameterFloat>  (apvts, "decay");
    sustainParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "sustain");
    releaseParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "release");
    gainParameter          = getTypedParameter<juce::AudioParameterFloat>  (apvts, "gain");
    widthParameter         = getTypedParameter<juce::AudioParameterFloat>  (apvts, "width");
    seqOnParameter         = getTypedParameter<juce::AudioParameterBool>   (apvts, "seqOn");
    seqSyncParameter       = getTypedParameter<juce::AudioParameterBool>   (apvts, "seqSync");
    seqTempoParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "seqTempo");
    seqRateParameter       = getTypedParameter<juce::AudioParameterChoice> (apvts, "seqRate");
    seqSwingParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "seqSwing");
    seqGateParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "seqGate");
    seqModeParameter       = getTypedParameter<juce::AudioParameterChoice> (apvts, "seqMode");
    sourceParameter        = getTypedParameter<juce::AudioParameterChoice> (apvts, "source");
    smpRootParameter       = getTypedParameter<juce::AudioParameterInt>    (apvts, "smpRoot");
    smpStartParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "smpStart");
    smpLoopParameter       = getTypedParameter<juce::AudioParameterChoice> (apvts, "smpLoop");
    smpLoopStartParameter  = getTypedParameter<juce::AudioParameterFloat>  (apvts, "smpLoopStart");
    smpLoopEndParameter    = getTypedParameter<juce::AudioParameterFloat>  (apvts, "smpLoopEnd");
    smpXfadeParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "smpXfade");
    grPosParameter         = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grPos");
    grScanParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grScan");
    grSizeParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grSize");
    grDensityParameter     = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grDensity");
    grSprayParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grSpray");
    grJitterParameter      = getTypedParameter<juce::AudioParameterFloat>  (apvts, "grJitter");
    chopOvertoneParameter  = getTypedParameter<juce::AudioParameterFloat>  (apvts, "chopOvertone");
    chopCombParameter      = getTypedParameter<juce::AudioParameterInt>    (apvts, "chopComb");
    noiseLevelParameter    = getTypedParameter<juce::AudioParameterFloat>  (apvts, "noiseLevel");
    foldDriveParameter     = getTypedParameter<juce::AudioParameterFloat>  (apvts, "foldDrive");
    foldMixParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "foldMix");

    loadDefaultPattern();
    publishSample (SampleLoader::makeDefaultSample());

    if (const auto path = findMidiLogPath (wrapperType); path.isNotEmpty())
        midiLogger.start (midiLogFileForNewInstance (path));
}

NeonSynthProcessor::~NeonSynthProcessor()
{
    sampleLoader.removeAllJobs (true, 10000);   // a running load finishes before the slot it publishes into goes away
}

void NeonSynthProcessor::reset()
{
    // Repeating a render from here gives the same grains (hosts call this before offline renders).
    for (auto* v : voices)
        v->resetSeeds();
}

void NeonSynthProcessor::releaseResources()
{
    sampleSlot.setAudioRunning (false);   // nothing reads the sample while stopped: replaced ones can be freed now
}

void NeonSynthProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);

    for (auto* v : voices)
        v->prepare (sampleRate, samplesPerBlock);

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainParameter->get()));

    midiLogger.setSampleRate (sampleRate);

    sequencer.prepare (sampleRate);
    playMidi.ensureSize (8192);   // bytes; far more than a block of keyboard + sequencer events needs
    internalBeat   = 0.0;
    numHeldKeys    = 0;

    sampleSlot.setAudioRunning (true);
}

void NeonSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    // TODO: the plugin has no input bus, so the buffer arrives with no incoming
    // audio. Add an input bus if you want to mix or modulate external sound here.
    buffer.clear();
    midiLogger.push (midiMessages, samplesProcessed);   // lock-free; formatting/I/O happen elsewhere
    blockSample = sampleSlot.beginBlock();   // the voices' sample for this block (used by the sample sources from P2)

    NeonSynthVoice::Params params;
    params.waveform = waveformParameter->getIndex();
    params.cutoffHz = cutoffParameter->get();
    params.filterOn = filterEnabledParameter->get();
    params.width    = widthParameter->get();
    params.adsr     = { attackParameter->get(), decayParameter->get(),
                        sustainParameter->get(), releaseParameter->get() };

    params.source = (NeonSynthVoice::Source) sourceParameter->getIndex();
    params.sample = blockSample;
    params.sampleSettings = { smpRootParameter->get(), smpStartParameter->get(), smpLoopParameter->getIndex(),
                              smpLoopStartParameter->get(), smpLoopEndParameter->get(), smpXfadeParameter->get() * 0.001f };
    params.grainSettings  = { grPosParameter->get(), grScanParameter->get(), grSizeParameter->get() * 0.001f,
                              grDensityParameter->get(), grSprayParameter->get(), grJitterParameter->get() };
    params.chop = { chopPattern.load (std::memory_order_relaxed), chopCombParameter->get(), chopOvertoneParameter->get() * 0.01f };
    params.sculpt = { noiseLevelParameter->get() * 0.01f, foldDriveParameter->get(), foldMixParameter->get() * 0.01f };

    // Every voice shares the same settings. TODO: per-voice detune for unison, or extra harmonics.
    for (auto* v : voices)
        v->setParameters (params);

    // Voice assignment is automatic: juce::Synthesiser reads each note-on in
    // midiMessages, picks a free voice (stealing one if all 16 are busy)
    // and calls its startNote() with that note number and velocity.

    // Voices render straight into 'buffer', so any host block size is safe.
    // The synth plays the keyboard (filtered by the sequencer mode) plus the sequencer.
    buildPlayMidi (midiMessages, numSamples);
    synth.renderNextBlock (buffer, playMidi, 0, numSamples);

    outputGain.setTargetValue (juce::Decibels::decibelsToGain (gainParameter->get()));
    outputGain.applyGain (buffer, numSamples);

    // Headroom: 16 full-velocity voices can sum to about +12 dBFS. The soft
    // clipper keeps the output under -1 dBFS and is bit-transparent below -6 dBFS.
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* samples = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            samples[i] = softClip (samples[i]);
    }

    pushScope (buffer.getReadPointer (0),
               buffer.getReadPointer (buffer.getNumChannels() > 1 ? 1 : 0),   // mono bus: L == R
               numSamples);
    publishVoiceViews();
    samplesProcessed += numSamples;
}

void NeonSynthProcessor::pushScope (const float* left, const float* right, int numFrames) noexcept
{
    int i = 0;
    scopeFifo.write (numFrames).forEach ([&] (int index)
    {
        scopeRing[(size_t) (2 * index)]     = left[i];
        scopeRing[(size_t) (2 * index + 1)] = right[i];
        ++i;
    });
}

void NeonSynthProcessor::publishVoiceViews() noexcept
{
    for (size_t i = 0; i < voices.size() && i < voiceViews.size(); ++i)
    {
        const auto t = voices[i]->getTelemetry();
        auto& v = voiceViews[i];
        v.active         .store (t.active,          std::memory_order_relaxed);
        v.level          .store (t.level,           std::memory_order_relaxed);
        v.frequencyHz    .store (t.frequencyHz,     std::memory_order_relaxed);
        v.secondsSinceOn .store (t.secondsSinceOn,  std::memory_order_relaxed);
        v.secondsSinceOff.store (t.secondsSinceOff, std::memory_order_relaxed);
        v.samplePosition .store (t.samplePosition,  std::memory_order_relaxed);
        for (size_t g = 0; g < v.grainPositions.size(); ++g)
            v.grainPositions[g].store ((int) g < t.numGrains ? t.grainPositions[g] : -1.0f, std::memory_order_relaxed);
        v.playingRandom.store (t.playingRandom, std::memory_order_relaxed);
        if (t.playingRandom)
            for (size_t k = 0; k < v.randomLevels.size(); ++k)
                v.randomLevels[k].store (t.randomLevels[k], std::memory_order_relaxed);
    }
}

int NeonSynthProcessor::drainScope (float* destLeft, float* destRight, int maxFrames) noexcept
{
    int i = 0;
    scopeFifo.read (juce::jmin (maxFrames, scopeFifo.getNumReady())).forEach ([&] (int index)
    {
        destLeft[i]  = scopeRing[(size_t) (2 * index)];
        destRight[i] = scopeRing[(size_t) (2 * index + 1)];
        ++i;
    });
    return i;
}

bool NeonSynthProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

//==============================================================================
// Step sequencer

namespace
{
    // State tree layout for the pattern: <Pattern><Step on="1" note="48" velocity="110"/> x16</Pattern>
    const juce::Identifier patternId  { "Pattern" };
    const juce::Identifier stepId     { "Step" };
    const juce::Identifier onId       { "on" };
    const juce::Identifier noteId     { "note" };
    const juce::Identifier velocityId { "velocity" };

    /**
        Saved-state format version, written as an attribute of the state tree.
          (none) v1.0 - v1.3: parameters, the editor's view and, from v1.3, the pattern
          2      v1.4: same content; the attribute exists so later format changes
                 can tell old sessions apart and migrate them
          3      v1.5: adds the Sample child (the sample, embedded or by path);
                 older states load with the built-in pad
          4      v1.6+: adds the Chop child (CHOP's 16-step pattern); older
                 states load with the default pattern (every second step)
    */
    const juce::Identifier stateVersionId { "stateVersion" };
    constexpr int currentStateVersion = 4;

    // State tree layout for CHOP's pattern (v1.6.0+): <Chop pattern="0101010101010101"/>,
    // one character per step, step 1 first, "1" = chopped.
    const juce::Identifier chopId        { "Chop" };
    const juce::Identifier chopPatternId { "pattern" };

    juce::String chopPatternToString (juce::uint16 pattern)
    {
        juce::String text;
        for (int i = 0; i < Chop::stepsPerPattern; ++i)
            text << (((pattern >> i) & 1u) != 0 ? "1" : "0");
        return text;
    }

    juce::uint16 chopPatternFromString (const juce::String& text, juce::uint16 fallback)
    {
        if (text.length() != Chop::stepsPerPattern || ! text.containsOnly ("01"))
            return fallback;
        juce::uint16 pattern = 0;
        for (int i = 0; i < Chop::stepsPerPattern; ++i)
            if (text[i] == '1')
                pattern = (juce::uint16) (pattern | (1u << i));
        return pattern;
    }

    // State tree layout for the sample (v1.5.0+):
    //   <Sample default="1"/>                                  the built-in pad
    //   <Sample name="pad.wav" path="C:/..." hash="..." flac="<base64>"/>
    // flac is the sample itself as 24-bit FLAC, left out above 10 MB (the path is used then).
    const juce::Identifier sampleId        { "Sample" };
    const juce::Identifier sampleDefaultId { "default" };
    const juce::Identifier sampleNameId    { "name" };
    const juce::Identifier samplePathId    { "path" };
    const juce::Identifier sampleHashId    { "hash" };
    const juce::Identifier sampleFlacId    { "flac" };

    /** Keyboard input is played on this channel; the sequencer has StepSequencer::midiChannel (16). */
    constexpr int keyboardChannel = 1;
    static_assert (keyboardChannel != StepSequencer::midiChannel);

    /** Notes are transposed relative to this key in Transpose mode (C4). */
    constexpr int transposeReferenceNote = 60;
}

void NeonSynthProcessor::loadDefaultPattern() noexcept
{
    // A C-minor bassline around C3 with octave jumps; notes are kept on the
    // "off" steps too, so switching a step on gives a sensible pitch.
    const StepSequencer::Step pattern[StepSequencer::numSteps] =
    {
        { true, 48, 112 }, { false, 48, 90 }, { true, 60, 80 },  { true, 48, 100 },
        { true, 51, 112 }, { false, 51, 90 }, { true, 55, 90 },  { true, 58, 80 },
        { true, 48, 112 }, { false, 48, 90 }, { true, 60, 80 },  { true, 48, 100 },
        { true, 53, 112 }, { false, 53, 90 }, { true, 55, 90 },  { true, 51, 80 },
    };

    for (int i = 0; i < StepSequencer::numSteps; ++i)
        sequencer.setStep (i, pattern[i]);
}

void NeonSynthProcessor::setSequencerStep (int index, const StepSequencer::Step& step)
{
    sequencer.setStep (index, step);
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void NeonSynthProcessor::trackHeldKey (int note, bool isDown) noexcept
{
    // Remove the key if present, then (for a key-down) append it as the most recent.
    for (int i = 0; i < numHeldKeys; ++i)
    {
        if (heldKeys[(size_t) i] == note)
        {
            for (int j = i + 1; j < numHeldKeys; ++j)
                heldKeys[(size_t) (j - 1)] = heldKeys[(size_t) j];
            --numHeldKeys;
            break;
        }
    }

    if (isDown && numHeldKeys < (int) heldKeys.size())
        heldKeys[(size_t) numHeldKeys++] = note;
}

void NeonSynthProcessor::buildPlayMidi (const juce::MidiBuffer& input, int numSamples) noexcept
{
    playMidi.clear();

    const bool sequencerOn = seqOnParameter->get();
    const auto mode = sequencerOn ? (SequencerMode) seqModeParameter->getIndex() : SequencerMode::layer;

    if (mode != SequencerMode::transpose)
        numHeldKeys = 0;   // forget keys held under another mode

    // 1. Keyboard input. Layer passes everything through; Replace and Transpose
    //    keep controllers, pitch bend and so on, but take note-ons out of the
    //    synth's path (Transpose uses them to steer the pattern instead).
    //
    //    Note-offs reach the synth in every mode: a key pressed in Layer and
    //    released after switching to Replace or Transpose (or after turning the
    //    sequencer on) must still stop its voice. juce::Synthesiser ignores a
    //    note-off for a note that isn't sounding, so this is harmless.
    //    SysEx is dropped: the synth ignores it, and a large dump could
    //    outgrow playMidi's preallocated capacity on the audio thread.
    //
    //    Keyboard messages are moved to MIDI channel 1, whatever channel they
    //    arrived on. The sequencer owns channel 16; juce::Synthesiser matches
    //    note-offs by channel and note, so a keyboard on channel 16 would
    //    otherwise have its notes cut off by the sequencer playing the same
    //    pitch. The synth has no per-channel behaviour of its own, so nothing
    //    else changes (a sustain pedal still holds the keyboard's notes).
    for (const auto event : input)
    {
        if (event.numBytes <= 0 || event.data[0] == 0xf0)
            continue;   // empty or SysEx

        juce::uint8 bytes[3] = {};
        const int numBytes = juce::jmin (event.numBytes, 3);
        std::memcpy (bytes, event.data, (size_t) numBytes);

        if (event.numBytes <= 3 && bytes[0] >= 0x80 && bytes[0] < 0xf0)
            bytes[0] = (juce::uint8) ((bytes[0] & 0xf0) | (keyboardChannel - 1));

        const int  status  = numBytes >= 3 ? (bytes[0] & 0xf0) : 0;
        const bool noteOn  = status == 0x90 && bytes[2] > 0;
        const bool noteOff = status == 0x80 || (status == 0x90 && bytes[2] == 0);

        if ((noteOn || noteOff) && mode == SequencerMode::transpose)
            trackHeldKey (bytes[1], noteOn);

        if (! noteOn || mode == SequencerMode::layer)
        {
            if (event.numBytes <= 3)
                playMidi.addEvent (bytes, numBytes, event.samplePosition);
            else
                playMidi.addEvent (event.data, event.numBytes, event.samplePosition);   // longer system messages, unchanged
        }
    }

    // 2. Musical time: the host transport when Sync is on and the host reports
    //    one (a DAW), otherwise the internal clock at the Tempo parameter
    //    (the Standalone, or Sync off). The internal clock restarts at beat 0
    //    each time the sequencer is switched on.
    if (sequencerOn && ! wasSequencerOn)
        internalBeat = 0.0;
    wasSequencerOn = sequencerOn;

    StepSequencer::Transport transport;
    bool followsHost = false;

    if (seqSyncParameter->get())
    {
        if (auto* hostPlayHead = getPlayHead())
        {
            if (const auto position = hostPlayHead->getPosition(); position.hasValue())
            {
                const auto ppq = position->getPpqPosition();
                const auto bpm = position->getBpm();

                if (ppq.hasValue() && bpm.hasValue())
                {
                    transport   = { position->getIsPlaying(), *ppq, *bpm };
                    followsHost = true;
                }
            }
        }
    }

    if (! followsHost)
    {
        const double tempo = seqTempoParameter->get();
        transport = { true, internalBeat, tempo };
        internalBeat += (double) numSamples * tempo / (60.0 * getSampleRate());
    }

    transport.playing = transport.playing && sequencerOn;   // switching off releases sounding notes
    sequencerTempo.store ((float) transport.bpm, std::memory_order_relaxed);
    sequencerFollowsHost.store (followsHost, std::memory_order_relaxed);

    // 3. The sequencer's own events.
    StepSequencer::Settings settings;
    settings.rate  = (StepSequencer::Rate) seqRateParameter->getIndex();
    settings.swing = seqSwingParameter->get();
    settings.gate  = seqGateParameter->get();

    if (mode == SequencerMode::transpose)
    {
        // Steered by the most recently pressed key that is still held; silent with no key down.
        settings.triggersEnabled = numHeldKeys > 0;
        settings.transpose = numHeldKeys > 0 ? heldKeys[(size_t) (numHeldKeys - 1)] - transposeReferenceNote : 0;
    }

    sequencer.process (playMidi, numSamples, transport, settings);
}

//==============================================================================
// Sample

void NeonSynthProcessor::publishSample (std::unique_ptr<SampleData> sample, const SampleData::Origin* missing)
{
    {
        const juce::ScopedLock sl (sampleInfoLock);
        missingSample     = missing != nullptr ? *missing : SampleData::Origin{};
        missingSampleHash = {};
        sampleProblem     = missing != nullptr ? "sample missing: " + missing->name : juce::String();
    }
    sampleSlot.publish (std::move (sample));
}

void NeonSynthProcessor::loadSampleFileAsync (const juce::File& file)
{
    const int generation = ++sampleLoadGeneration;
    sampleLoading = true;

    sampleLoader.addJob ([this, file, generation]
    {
        auto result = SampleLoader::loadFile (file);

        if (generation != sampleLoadGeneration.load())
            return;   // a newer load was requested meanwhile

        if (result.sample != nullptr)
            publishSample (std::move (result.sample));
        else
        {
            const juce::ScopedLock sl (sampleInfoLock);
            sampleProblem = file.getFileName() + ": " + result.error;
        }

        sampleLoading = false;
        updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    });
}

bool NeonSynthProcessor::loadSampleFile (const juce::File& file, juce::String& error)
{
    ++sampleLoadGeneration;   // supersedes any background load still running
    auto result = SampleLoader::loadFile (file);

    if (result.sample == nullptr)
    {
        error = result.error;
        return false;
    }

    publishSample (std::move (result.sample));
    return true;
}

void NeonSynthProcessor::useDefaultSample()
{
    ++sampleLoadGeneration;
    publishSample (SampleLoader::makeDefaultSample());
}

NeonSynthProcessor::SampleStatus NeonSynthProcessor::getSampleStatus() const
{
    SampleStatus status;
    sampleSlot.withCurrent ([&] (const SampleData* sample)
    {
        if (sample != nullptr)
        {
            status.name    = sample->getOrigin().name;
            status.seconds    = sample->getLengthSeconds();
            status.sampleRate = sample->getSampleRate();
            status.path       = sample->getOrigin().path;
        }
    });

    const juce::ScopedLock sl (sampleInfoLock);
    status.problem = sampleProblem;
    status.loading = sampleLoading.load();
    return status;
}

//==============================================================================
// State

void NeonSynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Parameters and GUI state come from the APVTS; the pattern is added as a
    // child tree because its 16 steps are not host parameters.
    auto state = apvts.copyState();
    state.setProperty (stateVersionId, currentStateVersion, nullptr);

    if (auto previous = state.getChildWithName (patternId); previous.isValid())
        state.removeChild (previous, nullptr);

    juce::ValueTree pattern (patternId);
    for (int i = 0; i < StepSequencer::numSteps; ++i)
    {
        const auto s = sequencer.getStep (i);
        juce::ValueTree step (stepId);
        step.setProperty (onId,       s.on,       nullptr);
        step.setProperty (noteId,     s.note,     nullptr);
        step.setProperty (velocityId, s.velocity, nullptr);
        pattern.appendChild (step, nullptr);
    }
    state.appendChild (pattern, nullptr);

    if (auto previous = state.getChildWithName (chopId); previous.isValid())
        state.removeChild (previous, nullptr);
    juce::ValueTree chop (chopId);
    chop.setProperty (chopPatternId, chopPatternToString (chopPattern.load()), nullptr);
    state.appendChild (chop, nullptr);

    if (auto previous = state.getChildWithName (sampleId); previous.isValid())
        state.removeChild (previous, nullptr);

    juce::ValueTree sampleTree (sampleId);
    sampleSlot.withCurrent ([&] (const SampleData* sample)
    {
        const juce::ScopedLock sl (sampleInfoLock);

        if (missingSample.path.isNotEmpty())
        {
            // The session's sample couldn't be found: keep pointing at it, so saving doesn't lose it.
            sampleTree.setProperty (sampleNameId, missingSample.name, nullptr);
            sampleTree.setProperty (samplePathId, missingSample.path, nullptr);
            if (missingSampleHash.isNotEmpty())
                sampleTree.setProperty (sampleHashId, missingSampleHash, nullptr);
        }
        else if (sample == nullptr || sample->getOrigin().isDefault)
        {
            sampleTree.setProperty (sampleDefaultId, true, nullptr);
        }
        else
        {
            sampleTree.setProperty (sampleNameId, sample->getOrigin().name, nullptr);
            sampleTree.setProperty (samplePathId, sample->getOrigin().path, nullptr);
            sampleTree.setProperty (sampleHashId, sample->getHash(), nullptr);
            if (sample->getEmbeddedFlac().getSize() > 0)
                sampleTree.setProperty (sampleFlacId, sample->getEmbeddedFlac().toBase64Encoding(), nullptr);
        }
    });
    state.appendChild (sampleTree, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void NeonSynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    const auto tree = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (tree);

    // A state without a pattern (sessions saved before v1.3, or a preset that
    // only carries parameters) gets the default pattern, so loading it always
    // sounds the same whatever was loaded before.
    loadDefaultPattern();

    const auto pattern = tree.getChildWithName (patternId);
    for (int i = 0; i < juce::jmin (StepSequencer::numSteps, pattern.getNumChildren()); ++i)
    {
        const auto step = pattern.getChild (i);
        sequencer.setStep (i, { (bool) step.getProperty (onId, false),
                                (int)  step.getProperty (noteId, 60),
                                (int)  step.getProperty (velocityId, 100) });
    }

    // CHOP's pattern; states from before v1.6.0 have none and get the default.
    chopPattern.store (chopPatternFromString (tree.getChildWithName (chopId).getProperty (chopPatternId).toString(),
                                              ChopSettings().pattern));

    restoreSample (tree.getChildWithName (sampleId));
}

void NeonSynthProcessor::setChopStep (int step, bool chopped) noexcept
{
    if (step < 0 || step >= Chop::stepsPerPattern)
        return;
    const auto bit = (juce::uint16) (1u << step);
    auto current = chopPattern.load();
    while (! chopPattern.compare_exchange_weak (current, chopped ? (juce::uint16) (current | bit) : (juce::uint16) (current & ~bit)))
    {
    }
}

void NeonSynthProcessor::restoreSample (const juce::ValueTree& sampleTree)
{
    ++sampleLoadGeneration;   // a background load started before this state arrived must not win

    // No Sample child (states before v1.5.0) or the default: the built-in pad.
    if (! sampleTree.isValid() || (bool) sampleTree.getProperty (sampleDefaultId, false))
    {
        if (! sampleSlot.withCurrent ([] (const SampleData* current) { return current != nullptr && current->getOrigin().isDefault; }))
        {
            publishSample (SampleLoader::makeDefaultSample());
        }
        else
        {
            const juce::ScopedLock sl (sampleInfoLock);   // already the pad: just forget any missing-sample note
            missingSample = {};
            missingSampleHash = {};
            sampleProblem = {};
        }
        return;
    }

    const SampleData::Origin origin { sampleTree.getProperty (sampleNameId).toString(),
                                      sampleTree.getProperty (samplePathId).toString(), false };
    const auto hash = sampleTree.getProperty (sampleHashId).toString();

    // Already loaded (a host re-sending the same state): nothing to do.
    if (sampleSlot.withCurrent ([&] (const SampleData* current)
            { return current != nullptr && hash.isNotEmpty() && current->getHash() == hash && ! current->getOrigin().isDefault; }))
    {
        const juce::ScopedLock sl (sampleInfoLock);
        missingSample = {};
        sampleProblem = {};
        return;
    }

    // 1. The embedded copy.
    if (const auto encoded = sampleTree.getProperty (sampleFlacId).toString(); encoded.isNotEmpty())
    {
        juce::MemoryBlock flac;
        if (flac.fromBase64Encoding (encoded))
        {
            if (auto result = SampleLoader::loadEmbeddedFlac (flac, origin); result.sample != nullptr)
            {
                publishSample (std::move (result.sample));
                return;
            }
        }
    }

    // 2. The file, where it was saved from.
    if (origin.path.isNotEmpty())
    {
        if (auto result = SampleLoader::loadFile (juce::File (origin.path)); result.sample != nullptr)
        {
            publishSample (std::move (result.sample));
            return;
        }
    }

    // 3. Neither: play the built-in pad, but remember the sample so saving keeps the reference.
    publishSample (SampleLoader::makeDefaultSample(), &origin);
    const juce::ScopedLock sl (sampleInfoLock);
    missingSampleHash = hash;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NeonSynthProcessor();
}
