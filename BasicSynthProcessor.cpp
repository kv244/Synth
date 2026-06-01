#include "BasicSynthProcessor.h"
#include "BasicSynthEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout BasicSynthAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterInt>   ("waveform",      "Waveform",       0,     3,        0));
    layout.add (std::make_unique<juce::AudioParameterFloat> ("cutoff",        "Cutoff",         20.0f, 20000.0f, 2000.0f));
    layout.add (std::make_unique<juce::AudioParameterBool>  ("filterEnabled", "Filter Enabled", true));

    // ADSR — time params use a log-skewed range so the knob feels linear
    layout.add (std::make_unique<juce::AudioParameterFloat> ("attack",  "Attack",
        juce::NormalisableRange<float> (0.001f, 5.0f,  0.001f, 0.3f), 0.01f));
    layout.add (std::make_unique<juce::AudioParameterFloat> ("decay",   "Decay",
        juce::NormalisableRange<float> (0.001f, 5.0f,  0.001f, 0.3f), 0.10f));
    layout.add (std::make_unique<juce::AudioParameterFloat> ("sustain", "Sustain", 0.0f, 1.0f, 0.7f));
    layout.add (std::make_unique<juce::AudioParameterFloat> ("release", "Release",
        juce::NormalisableRange<float> (0.001f, 10.0f, 0.001f, 0.3f), 0.30f));
    return layout;
}

BasicSynthAudioProcessor::BasicSynthAudioProcessor()
    : AudioProcessor (BusesProperties()
                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    synth.clearVoices();
    for (int i = 0; i < 8; ++i)
        synth.addVoice (new BasicSynthVoice());
    synth.clearSounds();
    synth.addSound (new BasicSynthSound());

    waveformParameter      = dynamic_cast<juce::AudioParameterInt*>   (apvts.getParameter ("waveform"));
    cutoffParameter        = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("cutoff"));
    filterEnabledParameter = dynamic_cast<juce::AudioParameterBool*>  (apvts.getParameter ("filterEnabled"));
    attackParameter        = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("attack"));
    decayParameter         = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("decay"));
    sustainParameter       = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("sustain"));
    releaseParameter       = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("release"));
}

void BasicSynthAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);
    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<BasicSynthVoice*> (synth.getVoice (i)))
        {
            v->prepareToPlay (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
            v->setLastSampleRate (sampleRate);
        }
}

void BasicSynthAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    buffer.clear();

    int   waveform      = waveformParameter->get();
    float cutoff        = cutoffParameter->get();
    bool  filterEnabled = filterEnabledParameter->get();
    float attack        = attackParameter->get();
    float decay         = decayParameter->get();
    float sustain       = sustainParameter->get();
    float release       = releaseParameter->get();

    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<BasicSynthVoice*> (synth.getVoice (i)))
        {
            v->setWaveform      (waveform);
            v->setFilterCutoff  (cutoff);
            v->setFilterEnabled (filterEnabled);
            v->setADSR          (attack, decay, sustain, release);
        }

    synth.renderNextBlock (buffer, midiMessages, 0, buffer.getNumSamples());
    waveformVisualiser.pushBuffer (buffer);
}

bool BasicSynthAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void BasicSynthAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void BasicSynthAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BasicSynthAudioProcessor();
}
