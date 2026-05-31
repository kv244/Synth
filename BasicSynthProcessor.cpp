#include "BasicSynthProcessor.h"
#include "BasicSynthEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout BasicSynthAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterInt>   ("waveform",       "Waveform",        0,     3,       0));
    layout.add (std::make_unique<juce::AudioParameterFloat> ("cutoff",         "Cutoff",          20.0f, 20000.0f, 2000.0f));
    layout.add (std::make_unique<juce::AudioParameterBool>  ("filterEnabled",  "Filter Enabled",  true));
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

    waveformParameter       = dynamic_cast<juce::AudioParameterInt*>   (apvts.getParameter ("waveform"));
    cutoffParameter         = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter ("cutoff"));
    filterEnabledParameter  = dynamic_cast<juce::AudioParameterBool*>  (apvts.getParameter ("filterEnabled"));
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

    int waveform        = waveformParameter->get();
    float cutoff        = cutoffParameter->get();
    bool filterEnabled  = filterEnabledParameter->get();
    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<BasicSynthVoice*> (synth.getVoice (i)))
        {
            v->setWaveform (waveform);
            v->setFilterCutoff (cutoff);
            v->setFilterEnabled (filterEnabled);
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
