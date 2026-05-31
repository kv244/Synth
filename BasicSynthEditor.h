#pragma once
#include <JuceHeader.h>
#include "BasicSynthProcessor.h"

class BasicSynthEditor : public juce::AudioProcessorEditor
{
public:
    BasicSynthEditor (BasicSynthAudioProcessor& p)
        : AudioProcessorEditor (&p), processor (p)
    {
        backgroundImage = juce::ImageCache::getFromMemory (BinaryData::gui_background_png, BinaryData::gui_background_pngSize);

        const auto cyan    = juce::Colour (0xff00ffff);
        const auto magenta = juce::Colour (0xffff00ff);

        addAndMakeVisible (waveformCombo);
        waveformCombo.addItem ("Sine", 1);
        waveformCombo.addItem ("Square", 2);
        waveformCombo.addItem ("Triangle", 3);
        waveformCombo.addItem ("Sawtooth", 4);
        
        waveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
            (processor.getAPVTS(), "waveform", waveformCombo);

        addAndMakeVisible (cutoffSlider);
        cutoffSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        cutoffSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);

        cutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
            (processor.getAPVTS(), "cutoff", cutoffSlider);

        addAndMakeVisible (filterToggle);
        filterToggle.setButtonText ("ON");
        filterToggle.setColour (juce::ToggleButton::textColourId,        magenta);
        filterToggle.setColour (juce::ToggleButton::tickColourId,        magenta);
        filterToggle.setColour (juce::ToggleButton::tickDisabledColourId, magenta.withAlpha (0.3f));

        filterToggleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
            (processor.getAPVTS(), "filterEnabled", filterToggle);

        filterToggle.onStateChange = [this] { updateFilterState(); };
        updateFilterState();

        waveformCombo.setColour (juce::ComboBox::outlineColourId, cyan);
        waveformCombo.setColour (juce::ComboBox::textColourId, cyan);
        waveformCombo.setColour (juce::ComboBox::arrowColourId, cyan);

        cutoffSlider.setColour (juce::Slider::thumbColourId, magenta);
        cutoffSlider.setColour (juce::Slider::trackColourId, magenta.withAlpha (0.3f));
        cutoffSlider.setColour (juce::Slider::backgroundColourId, juce::Colours::black);

        auto& vis = processor.waveformVisualiser;
        vis.setBufferSize (512);
        vis.setRepaintRate (30);
        vis.setColours (juce::Colours::black, cyan);
        addAndMakeVisible (vis);

        setSize (400, 365);
    }

    void updateFilterState()
    {
        bool on = filterToggle.getToggleState();
        cutoffSlider.setEnabled (on);
        cutoffSlider.setAlpha (on ? 1.0f : 0.4f);
    }

    void resized() override
    {
        waveformCombo.setBounds (30, 75, 340, 32);
        filterToggle.setBounds  (310, 130, 60, 20);
        cutoffSlider.setBounds  (30, 153, 340, 40);
        processor.waveformVisualiser.setBounds (30, 231, 340, 120);
    }

    void paint (juce::Graphics& g) override
    {
        if (backgroundImage.isValid())
            g.drawImageWithin (backgroundImage, 0, 0, getWidth(), getHeight(), juce::RectanglePlacement::fillDestination);
        else
            g.fillAll (juce::Colours::black);

        g.setFont (24.0f);
        g.setColour (juce::Colours::white);
        g.drawText ("NEON SYNTH", 0, 0, getWidth(), 48, juce::Justification::centred);

        g.setFont (13.0f);
        g.setColour (juce::Colour (0xff00ffff));
        g.drawText ("OSCILLATOR", 30, 52, 150, 20, juce::Justification::left);

        g.setColour (juce::Colour (0xffff00ff));
        g.drawText ("FILTER", 30, 130, 150, 20, juce::Justification::left);

        g.setColour (juce::Colour (0xff00ffff));
        g.drawText ("OUTPUT", 30, 208, 150, 20, juce::Justification::left);
    }

private:
    BasicSynthAudioProcessor& processor;
    juce::Image backgroundImage;

    juce::ComboBox    waveformCombo;
    juce::Slider      cutoffSlider;
    juce::ToggleButton filterToggle;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> waveformAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   cutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   filterToggleAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicSynthEditor)
};

inline juce::AudioProcessorEditor* BasicSynthAudioProcessor::createEditor()
{
    return new BasicSynthEditor (*this);
}
