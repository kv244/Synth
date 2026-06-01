#pragma once
#include <JuceHeader.h>
#include "BasicSynthProcessor.h"

class BasicSynthEditor : public juce::AudioProcessorEditor
{
public:
    BasicSynthEditor (BasicSynthAudioProcessor& p)
        : AudioProcessorEditor (&p), processor (p)
    {
        backgroundImage = juce::ImageCache::getFromMemory (
            BinaryData::gui_background_png, BinaryData::gui_background_pngSize);

        const auto cyan    = juce::Colour (0xff00ffff);
        const auto magenta = juce::Colour (0xffff00ff);
        const auto amber   = juce::Colour (0xffffcc00);

        // ── Oscillator ───────────────────────────────────────────────────────
        addAndMakeVisible (waveformCombo);
        waveformCombo.addItem ("Sine",     1);
        waveformCombo.addItem ("Square",   2);
        waveformCombo.addItem ("Triangle", 3);
        waveformCombo.addItem ("Sawtooth", 4);
        waveformCombo.setColour (juce::ComboBox::outlineColourId, cyan);
        waveformCombo.setColour (juce::ComboBox::textColourId,    cyan);
        waveformCombo.setColour (juce::ComboBox::arrowColourId,   cyan);
        waveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
            (processor.getAPVTS(), "waveform", waveformCombo);

        // ── ADSR knobs ───────────────────────────────────────────────────────
        auto setupKnob = [&] (juce::Slider& s)
        {
            s.setSliderStyle (juce::Slider::Rotary);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.setColour (juce::Slider::rotarySliderFillColourId,    amber);
            s.setColour (juce::Slider::rotarySliderOutlineColourId, amber.withAlpha (0.3f));
            s.setColour (juce::Slider::thumbColourId,               amber);
            addAndMakeVisible (s);
        };

        setupKnob (attackKnob);
        setupKnob (decayKnob);
        setupKnob (sustainKnob);
        setupKnob (releaseKnob);

        attackAttachment  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
            (processor.getAPVTS(), "attack",  attackKnob);
        decayAttachment   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
            (processor.getAPVTS(), "decay",   decayKnob);
        sustainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
            (processor.getAPVTS(), "sustain", sustainKnob);
        releaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
            (processor.getAPVTS(), "release", releaseKnob);

        // ── Filter ───────────────────────────────────────────────────────────
        addAndMakeVisible (cutoffSlider);
        cutoffSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        cutoffSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        cutoffSlider.setColour (juce::Slider::thumbColourId,       magenta);
        cutoffSlider.setColour (juce::Slider::trackColourId,       magenta.withAlpha (0.3f));
        cutoffSlider.setColour (juce::Slider::backgroundColourId,  juce::Colours::black);
        cutoffAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
            (processor.getAPVTS(), "cutoff", cutoffSlider);

        addAndMakeVisible (filterToggle);
        filterToggle.setButtonText ("ON");
        filterToggle.setColour (juce::ToggleButton::textColourId,         magenta);
        filterToggle.setColour (juce::ToggleButton::tickColourId,         magenta);
        filterToggle.setColour (juce::ToggleButton::tickDisabledColourId, magenta.withAlpha (0.3f));
        filterToggleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
            (processor.getAPVTS(), "filterEnabled", filterToggle);
        filterToggle.onStateChange = [this] { updateFilterState(); };
        updateFilterState();

        // ── Output visualiser ────────────────────────────────────────────────
        auto& vis = processor.waveformVisualiser;
        vis.setBufferSize (512);
        vis.setRepaintRate (30);
        vis.setColours (juce::Colours::black, cyan);
        addAndMakeVisible (vis);

        setSize (400, 430);
    }

    void updateFilterState()
    {
        bool on = filterToggle.getToggleState();
        cutoffSlider.setEnabled (on);
        cutoffSlider.setAlpha (on ? 1.0f : 0.4f);
    }

    void resized() override
    {
        // Oscillator
        waveformCombo.setBounds (30, 75, 340, 30);

        // ADSR knobs — four equal columns across the content area
        attackKnob .setBounds (30,  130, 70, 60);
        decayKnob  .setBounds (120, 130, 70, 60);
        sustainKnob.setBounds (210, 130, 70, 60);
        releaseKnob.setBounds (300, 130, 70, 60);

        // Filter
        filterToggle.setBounds (310, 212, 60, 20);
        cutoffSlider.setBounds (30,  235, 340, 38);

        // Output visualiser
        processor.waveformVisualiser.setBounds (30, 300, 340, 115);
    }

    void paint (juce::Graphics& g) override
    {
        if (backgroundImage.isValid())
            g.drawImageWithin (backgroundImage, 0, 0, getWidth(), getHeight(),
                               juce::RectanglePlacement::fillDestination);
        else
            g.fillAll (juce::Colours::black);

        // Title
        g.setFont (24.0f);
        g.setColour (juce::Colours::white);
        g.drawText ("NEON SYNTH", 0, 0, getWidth(), 48, juce::Justification::centred);

        g.setFont (13.0f);

        // Oscillator section
        g.setColour (juce::Colour (0xff00ffff));
        g.drawText ("OSCILLATOR", 30, 52, 150, 20, juce::Justification::left);

        // Envelope section
        g.setColour (juce::Colour (0xffffcc00));
        g.drawText ("ENVELOPE", 30, 108, 150, 20, juce::Justification::left);

        // Knob labels (centred under each knob)
        g.setFont (11.0f);
        g.drawText ("ATK", 30,  192, 70, 16, juce::Justification::centred);
        g.drawText ("DEC", 120, 192, 70, 16, juce::Justification::centred);
        g.drawText ("SUS", 210, 192, 70, 16, juce::Justification::centred);
        g.drawText ("REL", 300, 192, 70, 16, juce::Justification::centred);

        // Filter section
        g.setFont (13.0f);
        g.setColour (juce::Colour (0xffff00ff));
        g.drawText ("FILTER", 30, 212, 150, 20, juce::Justification::left);

        // Output section
        g.setColour (juce::Colour (0xff00ffff));
        g.drawText ("OUTPUT", 30, 280, 150, 20, juce::Justification::left);
    }

private:
    BasicSynthAudioProcessor& processor;
    juce::Image               backgroundImage;

    // Oscillator
    juce::ComboBox waveformCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> waveformAttachment;

    // ADSR
    juce::Slider attackKnob, decayKnob, sustainKnob, releaseKnob;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attackAttachment,
                                                                          decayAttachment,
                                                                          sustainAttachment,
                                                                          releaseAttachment;

    // Filter
    juce::Slider       cutoffSlider;
    juce::ToggleButton filterToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> cutoffAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> filterToggleAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicSynthEditor)
};

inline juce::AudioProcessorEditor* BasicSynthAudioProcessor::createEditor()
{
    return new BasicSynthEditor (*this);
}
