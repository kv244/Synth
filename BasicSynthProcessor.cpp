#include "BasicSynthProcessor.h"
#include "BasicSynthEditor.h"

namespace
{
    constexpr int numVoices = 16;

    /** Log path from BASICSYNTH_MIDILOG, or (Standalone only) from "--midilog <file>". */
    juce::String findMidiLogPath (juce::AudioProcessor::WrapperType wrapper)
    {
        auto path = juce::SystemStats::getEnvironmentVariable ("BASICSYNTH_MIDILOG", {});

        if (path.isEmpty() && wrapper == juce::AudioProcessor::wrapperType_Standalone)
        {
            const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
            const auto index = args.indexOf ("--midilog");

            if (index >= 0 && index + 1 < args.size())
                path = args[index + 1].unquoted();
        }

        return path;
    }

    template <typename ParamType>
    ParamType* getTypedParameter (juce::AudioProcessorValueTreeState& state, const char* id)
    {
        auto* p = dynamic_cast<ParamType*> (state.getParameter (id));
        jassert (p != nullptr);   // wrong ID or wrong type
        return p;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout BasicSynthAudioProcessor::createParameterLayout()
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
    return layout;
}

BasicSynthAudioProcessor::BasicSynthAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    for (int i = 0; i < numVoices; ++i)
        voices.push_back (static_cast<BasicSynthVoice*> (synth.addVoice (new BasicSynthVoice())));

    synth.addSound (new BasicSynthSound());

    waveformParameter      = getTypedParameter<juce::AudioParameterChoice> (apvts, "waveform");
    cutoffParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "cutoff");
    filterEnabledParameter = getTypedParameter<juce::AudioParameterBool>   (apvts, "filterEnabled");
    attackParameter        = getTypedParameter<juce::AudioParameterFloat>  (apvts, "attack");
    decayParameter         = getTypedParameter<juce::AudioParameterFloat>  (apvts, "decay");
    sustainParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "sustain");
    releaseParameter       = getTypedParameter<juce::AudioParameterFloat>  (apvts, "release");
    gainParameter          = getTypedParameter<juce::AudioParameterFloat>  (apvts, "gain");

    if (const auto path = findMidiLogPath (wrapperType); path.isNotEmpty())
        midiLogger.start (juce::File::getCurrentWorkingDirectory().getChildFile (path));
}

void BasicSynthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);

    for (auto* v : voices)
        v->prepare (sampleRate, samplesPerBlock);

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainParameter->get()));

    midiLogger.setSampleRate (sampleRate);
}

void BasicSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    // TODO: the plugin has no input bus, so the buffer arrives with no incoming
    // audio. Add an input bus if you want to mix or modulate external sound here.
    buffer.clear();
    midiLogger.push (midiMessages, samplesProcessed);   // lock-free; formatting/I/O happen elsewhere

    BasicSynthVoice::Params params;
    params.waveform = waveformParameter->getIndex();
    params.cutoffHz = cutoffParameter->get();
    params.filterOn = filterEnabledParameter->get();
    params.adsr     = { attackParameter->get(), decayParameter->get(),
                        sustainParameter->get(), releaseParameter->get() };

    // Every voice shares the same settings. TODO: per-voice detune for unison, or extra harmonics.
    for (auto* v : voices)
        v->setParameters (params);

    // Voice assignment is automatic: juce::Synthesiser reads each note-on in
    // midiMessages, picks a free voice (stealing one if all 16 are busy)
    // and calls its startNote() with that note number and velocity.

    // Voices render straight into 'buffer', so any host block size is safe.
    synth.renderNextBlock (buffer, midiMessages, 0, numSamples);

    outputGain.setTargetValue (juce::Decibels::decibelsToGain (gainParameter->get()));
    outputGain.applyGain (buffer, numSamples);

    pushScope (buffer.getReadPointer (0), numSamples);   // voices are mono: channel 0 is enough
    samplesProcessed += numSamples;
}

void BasicSynthAudioProcessor::pushScope (const float* samples, int numSamples) noexcept
{
    int i = 0;
    scopeFifo.write (numSamples).forEach ([&] (int index) { scopeRing[(size_t) index] = samples[i++]; });
}

int BasicSynthAudioProcessor::drainScope (float* dest, int maxSamples) noexcept
{
    int i = 0;
    scopeFifo.read (juce::jmin (maxSamples, scopeFifo.getNumReady()))
             .forEach ([&] (int index) { dest[i++] = scopeRing[(size_t) index]; });
    return i;
}

bool BasicSynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void BasicSynthAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void BasicSynthAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes); xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BasicSynthAudioProcessor();
}
