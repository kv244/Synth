#pragma once
#include <JuceHeader.h>
#include "BasicSynthProcessor.h"
#include "Visualisers.h"
#include "SequencerView.h"

/**
    Neon Synth editor.

    Laid out on a fixed 760 x 520 design grid and scaled uniformly, so the
    window can be resized (fixed aspect ratio) without any layout logic beyond
    one scale factor.

    Left column: oscillator, envelope (shape + knobs), filter, voice lights.
    Right column: scope / spectrum, stereo meter, output gain and stereo width.
    Bottom strip: the step sequencer - run / sync / tempo / rate / mode on the
    first row, the 16-step grid, then swing and gate.

    A 30 Hz timer drains the processor's lock-free scope FIFO and reads the
    per-voice atomics; it is the only path from audio to GUI.
*/
class BasicSynthEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit BasicSynthEditor (BasicSynthAudioProcessor& p)
        : AudioProcessorEditor (&p), processor (p), apvts (p.getAPVTS()), stepGrid (p)
    {
        backgroundImage = juce::ImageCache::getFromMemory (BinaryData::gui_background_png,
                                                           BinaryData::gui_background_pngSize);

        attackValue  = apvts.getRawParameterValue ("attack");
        decayValue   = apvts.getRawParameterValue ("decay");
        sustainValue = apvts.getRawParameterValue ("sustain");
        releaseValue = apvts.getRawParameterValue ("release");
        cutoffValue  = apvts.getRawParameterValue ("cutoff");
        filterValue  = apvts.getRawParameterValue ("filterEnabled");
        gainValue    = apvts.getRawParameterValue ("gain");
        widthValue   = apvts.getRawParameterValue ("width");
        seqTempoValue = apvts.getRawParameterValue ("seqTempo");
        seqSwingValue = apvts.getRawParameterValue ("seqSwing");
        seqGateValue  = apvts.getRawParameterValue ("seqGate");

        // ── Oscillator ───────────────────────────────────────────────────────
        addAndMakeVisible (waveformCombo);
        waveformCombo.addItem ("Sine",     1);
        waveformCombo.addItem ("Square",   2);
        waveformCombo.addItem ("Triangle", 3);
        waveformCombo.addItem ("Sawtooth", 4);
        waveformCombo.setColour (juce::ComboBox::outlineColourId, neon::cyan);
        waveformCombo.setColour (juce::ComboBox::textColourId,    neon::cyan);
        waveformCombo.setColour (juce::ComboBox::arrowColourId,   neon::cyan);
        waveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
            (apvts, "waveform", waveformCombo);

        // ── Envelope ─────────────────────────────────────────────────────────
        addAndMakeVisible (adsrView);

        for (auto* knob : { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob })
        {
            knob->setSliderStyle (juce::Slider::Rotary);
            knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::amber);
            knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::amber.withAlpha (0.3f));
            knob->setColour (juce::Slider::thumbColourId,               neon::amber);
            addAndMakeVisible (knob);
        }
        attackAttachment  = std::make_unique<SliderAttachment> (apvts, "attack",  attackKnob);
        decayAttachment   = std::make_unique<SliderAttachment> (apvts, "decay",   decayKnob);
        sustainAttachment = std::make_unique<SliderAttachment> (apvts, "sustain", sustainKnob);
        releaseAttachment = std::make_unique<SliderAttachment> (apvts, "release", releaseKnob);

        for (auto* label : { &attackReadout, &decayReadout, &sustainReadout, &releaseReadout })
            setUpReadout (*label, neon::amber, juce::Justification::centred);

        // ── Filter ───────────────────────────────────────────────────────────
        setUpLinearSlider (cutoffSlider, neon::magenta);
        cutoffAttachment = std::make_unique<SliderAttachment> (apvts, "cutoff", cutoffSlider);
        setUpReadout (cutoffReadout, neon::magenta, juce::Justification::centredRight);

        addAndMakeVisible (filterToggle);
        filterToggle.setButtonText ("ON");
        filterToggle.setColour (juce::ToggleButton::textColourId,         neon::magenta);
        filterToggle.setColour (juce::ToggleButton::tickColourId,         neon::magenta);
        filterToggle.setColour (juce::ToggleButton::tickDisabledColourId, neon::magenta.withAlpha (0.3f));
        filterToggleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
            (apvts, "filterEnabled", filterToggle);

        // Follows clicks and host automation alike; callbacks arrive on the message thread.
        filterEnabledListener = std::make_unique<juce::ParameterAttachment> (
            *apvts.getParameter ("filterEnabled"),
            [this] (float value) { updateFilterState (value >= 0.5f); });
        filterEnabledListener->sendInitialUpdate();

        // ── Voices ───────────────────────────────────────────────────────────
        addAndMakeVisible (voiceLeds);

        // ── Output: display, meter, gain, width ──────────────────────────────
        addChildComponent (oscilloscope);
        addChildComponent (spectrum);
        addAndMakeVisible (meter);

        for (auto* button : { &scopeButton, &spectrumButton })
        {
            button->setClickingTogglesState (true);
            button->setRadioGroupId (1);
            button->setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
            button->setColour (juce::TextButton::buttonOnColourId, neon::cyan.withAlpha (0.25f));
            button->setColour (juce::TextButton::textColourOffId,  neon::cyan.withAlpha (0.55f));
            button->setColour (juce::TextButton::textColourOnId,   neon::cyan);
            button->setColour (juce::ComboBox::outlineColourId,    neon::cyan.withAlpha (0.5f));
            addAndMakeVisible (button);
        }
        scopeButton.setConnectedEdges (juce::Button::ConnectedOnRight);
        spectrumButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        scopeButton.onClick    = [this] { if (scopeButton.getToggleState())    setView (0); };
        spectrumButton.onClick = [this] { if (spectrumButton.getToggleState()) setView (1); };

        setUpLinearSlider (gainSlider, neon::cyan);
        gainAttachment = std::make_unique<SliderAttachment> (apvts, "gain", gainSlider);
        setUpReadout (gainReadout, neon::cyan, juce::Justification::centredRight);

        setUpLinearSlider (widthSlider, neon::cyan);
        widthAttachment = std::make_unique<SliderAttachment> (apvts, "width", widthSlider);
        setUpReadout (widthReadout, neon::cyan, juce::Justification::centredRight);

        // ── Step sequencer ───────────────────────────────────────────────────
        // RUN and SYNC are latching buttons (lit = on) rather than tick boxes,
        // so they read as transport controls. ButtonAttachment works with any
        // juce::Button whose clicks toggle its state.
        for (auto* toggle : { &seqOnButton, &seqSyncButton })
        {
            toggle->setClickingTogglesState (true);
            toggle->setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
            toggle->setColour (juce::TextButton::buttonOnColourId, neon::amber.withAlpha (0.30f));
            toggle->setColour (juce::TextButton::textColourOffId,  neon::amber.withAlpha (0.55f));
            toggle->setColour (juce::TextButton::textColourOnId,   neon::amber);
            toggle->setColour (juce::ComboBox::outlineColourId,    neon::amber.withAlpha (0.6f));
            addAndMakeVisible (toggle);
        }
        seqOnButton.setTooltip ("Run the step sequencer");
        seqSyncButton.setTooltip ("Follow the host's tempo and transport when it has one; otherwise use TEMPO");
        seqOnAttachment   = std::make_unique<ButtonAttachment> (apvts, "seqOn",   seqOnButton);
        seqSyncAttachment = std::make_unique<ButtonAttachment> (apvts, "seqSync", seqSyncButton);

        setUpLinearSlider (seqTempoSlider, neon::amber);
        seqTempoAttachment = std::make_unique<SliderAttachment> (apvts, "seqTempo", seqTempoSlider);
        setUpReadout (seqTempoReadout, neon::amber, juce::Justification::centredRight);

        for (auto* combo : { &seqRateCombo, &seqModeCombo })
        {
            combo->setColour (juce::ComboBox::outlineColourId, neon::amber);
            combo->setColour (juce::ComboBox::textColourId,    neon::amber);
            combo->setColour (juce::ComboBox::arrowColourId,   neon::amber);
            addAndMakeVisible (combo);
        }
        seqRateCombo.addItemList ({ "1/8", "1/16", "1/32", "1/8T", "1/16T" }, 1);   // same order as the parameter
        seqModeCombo.addItemList ({ "Layer", "Replace", "Transpose" }, 1);
        seqRateCombo.setTooltip ("Step length");
        seqModeCombo.setTooltip ("Layer: sequencer + keyboard.  Replace: keyboard ignored.  "
                                 "Transpose: the pattern plays while a key is held, shifted by (key - C4).");
        seqRateAttachment = std::make_unique<ComboBoxAttachment> (apvts, "seqRate", seqRateCombo);
        seqModeAttachment = std::make_unique<ComboBoxAttachment> (apvts, "seqMode", seqModeCombo);

        addAndMakeVisible (stepGrid);

        setUpLinearSlider (seqSwingSlider, neon::amber);
        seqSwingAttachment = std::make_unique<SliderAttachment> (apvts, "seqSwing", seqSwingSlider);
        setUpReadout (seqSwingReadout, neon::amber, juce::Justification::centredRight);

        setUpLinearSlider (seqGateSlider, neon::amber);
        seqGateAttachment = std::make_unique<SliderAttachment> (apvts, "seqGate", seqGateSlider);
        setUpReadout (seqGateReadout, neon::amber, juce::Justification::centredRight);

        // The chosen view is GUI state, saved with the plugin's state but not automatable.
        setView ((int) apvts.state.getProperty (viewPropertyId, 0));

        setResizable (true, true);
        setResizeLimits (baseWidth * 3 / 4, baseHeight * 3 / 4, baseWidth * 2, baseHeight * 2);
        getConstrainer()->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
        setSize (baseWidth, baseHeight);

        updateReadouts();
        startTimerHz (timerHz);
    }

    void resized() override
    {
        const float s = scale();

        // Pre-render the background at this size so repaints only blit it.
        scaledBackground = juce::Image (juce::Image::RGB, juce::jmax (1, getWidth()), juce::jmax (1, getHeight()), true);
        {
            juce::Graphics bg (scaledBackground);
            if (backgroundImage.isValid())
                bg.drawImageWithin (backgroundImage, 0, 0, getWidth(), getHeight(),
                                    juce::RectanglePlacement::fillDestination);
            else
                bg.fillAll (juce::Colours::black);
        }

        const auto place = [s] (juce::Component& c, float x, float y, float w, float h)
        {
            c.setBounds ((juce::Rectangle<float> (x, y, w, h) * s).toNearestInt());
        };

        // Left column
        place (waveformCombo, 30, 72, 340, 28);
        place (adsrView,      30, 130, 340, 64);

        const float knobX[] = { 30, 120, 210, 300 };
        juce::Slider* knobs[] = { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob };
        juce::Label*  reads[] = { &attackReadout, &decayReadout, &sustainReadout, &releaseReadout };
        for (int i = 0; i < 4; ++i)
        {
            place (*knobs[i], knobX[i], 198, 70, 48);
            place (*reads[i], knobX[i], 260, 70, 13);
        }

        place (cutoffReadout, 150, 281, 150, 18);
        place (filterToggle,  310, 280, 60, 20);
        place (cutoffSlider,  30, 302, 340, 28);
        place (voiceLeds,     30, 362, 340, 18);

        // Right column
        place (scopeButton,    588, 48, 70, 22);
        place (spectrumButton, 658, 48, 82, 22);
        place (oscilloscope,   400, 76, 300, 252);
        place (spectrum,       400, 76, 300, 252);
        place (meter,          708, 76, 32, 252);
        place (gainSlider,     452, 336, 228, 24);
        place (gainReadout,    680, 340, 60, 16);
        place (widthSlider,    452, 362, 228, 24);
        place (widthReadout,   680, 366, 60, 16);

        // Sequencer strip
        place (seqOnButton,     135, 400, 62, 22);
        place (seqSyncButton,   200, 400, 66, 22);
        place (seqTempoSlider,  316, 400, 150, 22);
        place (seqTempoReadout, 466, 403, 84, 16);
        place (seqRateCombo,    560, 400, 70, 22);
        place (seqModeCombo,    636, 400, 104, 22);
        place (stepGrid,        30, 430, 710, 50);
        place (seqSwingSlider,  80, 486, 230, 22);
        place (seqSwingReadout, 310, 489, 60, 16);
        place (seqGateSlider,   446, 486, 234, 22);
        place (seqGateReadout,  680, 489, 60, 16);

        for (auto* label : { &attackReadout, &decayReadout, &sustainReadout, &releaseReadout,
                             &cutoffReadout, &gainReadout, &widthReadout,
                             &seqTempoReadout, &seqSwingReadout, &seqGateReadout })
            label->setFont (neon::font (11.0f * s));
    }

    void paint (juce::Graphics& g) override
    {
        g.drawImageAt (scaledBackground, 0, 0);

        // Static labels are drawn on the 760 x 400 design grid, scaled.
        g.addTransform (juce::AffineTransform::scale (scale()));

        g.setFont (neon::font (24.0f));
        g.setColour (juce::Colours::white);
        g.drawText ("NEON SYNTH", 0, 6, baseWidth, 36, juce::Justification::centred);

        g.setFont (neon::font (13.0f));
        g.setColour (neon::cyan);
        g.drawText ("OSCILLATOR", 30, 50, 150, 20, juce::Justification::left);
        g.drawText ("VOICES",     30, 340, 150, 20, juce::Justification::left);
        g.drawText ("OUTPUT",     400, 50, 150, 20, juce::Justification::left);

        g.setColour (neon::amber);
        g.drawText ("ENVELOPE", 30, 108, 150, 20, juce::Justification::left);

        g.setColour (neon::magenta);
        g.drawText ("FILTER", 30, 280, 150, 20, juce::Justification::left);

        g.setFont (neon::font (11.0f));
        g.setColour (neon::cyan);
        g.drawText ("GAIN",  400, 338, 50, 20, juce::Justification::left);
        g.drawText ("WIDTH", 400, 364, 50, 20, juce::Justification::left);

        // Sequencer strip
        g.setColour (neon::amber.withAlpha (0.25f));
        g.drawHorizontalLine (394, 30.0f, 740.0f);
        g.setColour (neon::amber);
        g.setFont (neon::font (13.0f));
        g.drawText ("SEQUENCER", 30, 401, 100, 20, juce::Justification::left);
        g.setFont (neon::font (11.0f));
        g.drawText ("TEMPO", 272, 401, 44, 20, juce::Justification::left);
        g.drawText ("SWING", 30,  487, 50, 20, juce::Justification::left);
        g.drawText ("GATE",  400, 487, 46, 20, juce::Justification::left);

        g.setColour (neon::amber);
        const char* names[] = { "ATK", "DEC", "SUS", "REL" };
        const int   knobX[] = { 30, 120, 210, 300 };
        for (int i = 0; i < 4; ++i)
            g.drawText (names[i], knobX[i], 247, 70, 13, juce::Justification::centred);
    }

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    static constexpr int baseWidth  = 760;
    static constexpr int baseHeight = 520;
    static constexpr int timerHz    = 30;
    static inline const juce::Identifier viewPropertyId { "view" };

    float scale() const noexcept { return (float) getWidth() / (float) baseWidth; }

    void setUpLinearSlider (juce::Slider& slider, juce::Colour colour)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setColour (juce::Slider::thumbColourId,      colour);
        slider.setColour (juce::Slider::trackColourId,      colour.withAlpha (0.3f));
        slider.setColour (juce::Slider::backgroundColourId, juce::Colours::black);
        addAndMakeVisible (slider);
    }

    void setUpReadout (juce::Label& label, juce::Colour colour, juce::Justification justification)
    {
        label.setColour (juce::Label::textColourId, colour);
        label.setJustificationType (justification);
        label.setInterceptsMouseClicks (false, false);
        label.setBorderSize ({});
        addAndMakeVisible (label);
    }

    void updateFilterState (bool on)
    {
        cutoffSlider.setEnabled (on);
        cutoffSlider.setAlpha (on ? 1.0f : 0.4f);
        cutoffReadout.setAlpha (on ? 1.0f : 0.4f);
    }

    void setView (int view)
    {
        view = juce::jlimit (0, 1, view);
        oscilloscope.setVisible (view == 0);
        spectrum.setVisible (view == 1);
        scopeButton.setToggleState (view == 0, juce::dontSendNotification);
        spectrumButton.setToggleState (view == 1, juce::dontSendNotification);
        apvts.state.setProperty (viewPropertyId, view, nullptr);
    }

    void updateReadouts()
    {
        const auto set = [] (juce::Label& label, const juce::String& text)
        {
            label.setText (text, juce::dontSendNotification);   // repaints only if the text changed
        };

        set (attackReadout,  neon::formatTime (attackValue->load()));
        set (decayReadout,   neon::formatTime (decayValue->load()));
        set (sustainReadout, juce::String (juce::roundToInt (sustainValue->load() * 100.0f)) + "%");
        set (releaseReadout, neon::formatTime (releaseValue->load()));
        set (cutoffReadout,  neon::formatHz (cutoffValue->load()));

        const float gainDb = gainValue->load();
        set (gainReadout,  (gainDb > 0.0f ? "+" : "") + juce::String (gainDb, 1) + " dB");
        set (widthReadout, juce::String (juce::roundToInt (widthValue->load() * 100.0f)) + "%");

        // Shows the tempo actually in use: the host's while following it, otherwise the TEMPO slider.
        set (seqTempoReadout, processor.isSequencerFollowingHost()
                                  ? "HOST " + juce::String (processor.getSequencerTempo(), 1)
                                  : juce::String (seqTempoValue->load(), 1) + " BPM");
        set (seqSwingReadout, juce::String (juce::roundToInt (seqSwingValue->load() * 100.0f)) + "%");
        set (seqGateReadout,  juce::String (juce::roundToInt (seqGateValue->load() * 100.0f)) + "%");
    }

    void timerCallback() override
    {
        const int n = processor.drainScope (scratchLeft.data(), scratchRight.data(), (int) scratchLeft.size());

        // Scope and spectrum show the mid signal (L + R) / 2.
        for (int i = 0; i < n; ++i)
            scratchMid[(size_t) i] = 0.5f * (scratchLeft[(size_t) i] + scratchRight[(size_t) i]);
        history.push (scratchMid.data(), n);

        meter.update (scratchLeft.data(), scratchRight.data(), n, timerHz);

        const auto& voices = processor.getVoiceViews();
        float lowestHz = 0.0f;
        for (const auto& v : voices)
        {
            const float hz = v.frequencyHz.load (std::memory_order_relaxed);
            if (v.active.load (std::memory_order_relaxed) && hz > 0.0f && (lowestHz == 0.0f || hz < lowestHz))
                lowestHz = hz;
        }

        const double sampleRate = processor.getSampleRate();
        if (oscilloscope.isVisible())
            oscilloscope.update (history, sampleRate, lowestHz);
        else
            spectrum.update (history, sampleRate, cutoffValue->load(), filterValue->load() >= 0.5f);

        adsrView.update (attackValue->load(), decayValue->load(), sustainValue->load(), releaseValue->load(), voices);
        voiceLeds.update (voices);
        stepGrid.refresh();   // pattern + playhead
        updateReadouts();
    }

    BasicSynthAudioProcessor&           processor;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Image backgroundImage, scaledBackground;

    std::atomic<float>* attackValue = nullptr;
    std::atomic<float>* decayValue = nullptr;
    std::atomic<float>* sustainValue = nullptr;
    std::atomic<float>* releaseValue = nullptr;
    std::atomic<float>* cutoffValue = nullptr;
    std::atomic<float>* filterValue = nullptr;
    std::atomic<float>* gainValue = nullptr;
    std::atomic<float>* widthValue = nullptr;
    std::atomic<float>* seqTempoValue = nullptr;
    std::atomic<float>* seqSwingValue = nullptr;
    std::atomic<float>* seqGateValue = nullptr;

    // Visualisers and their data
    SignalHistory    history;
    OscilloscopeView oscilloscope;
    SpectrumView     spectrum;
    LevelMeter       meter;
    AdsrView         adsrView;
    VoiceLeds        voiceLeds;
    std::array<float, BasicSynthAudioProcessor::scopeFifoSize> scratchLeft {}, scratchRight {}, scratchMid {};

    juce::TextButton scopeButton { "SCOPE" }, spectrumButton { "SPECTRUM" };
    juce::TooltipWindow tooltipWindow { this };

    // Controls
    juce::ComboBox waveformCombo;
    juce::Slider   attackKnob, decayKnob, sustainKnob, releaseKnob;
    juce::Slider   cutoffSlider, gainSlider, widthSlider;
    juce::ToggleButton filterToggle;
    juce::Label attackReadout, decayReadout, sustainReadout, releaseReadout,
                cutoffReadout, gainReadout, widthReadout;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> waveformAttachment;
    std::unique_ptr<SliderAttachment> attackAttachment, decayAttachment, sustainAttachment, releaseAttachment,
                                      cutoffAttachment, gainAttachment, widthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> filterToggleAttachment;

    // Step sequencer
    StepGrid           stepGrid;
    juce::TextButton seqOnButton { "RUN" }, seqSyncButton { "SYNC" };
    juce::Slider       seqTempoSlider, seqSwingSlider, seqGateSlider;
    juce::ComboBox     seqRateCombo, seqModeCombo;
    juce::Label        seqTempoReadout, seqSwingReadout, seqGateReadout;
    std::unique_ptr<ButtonAttachment>   seqOnAttachment, seqSyncAttachment;
    std::unique_ptr<SliderAttachment>   seqTempoAttachment, seqSwingAttachment, seqGateAttachment;
    std::unique_ptr<ComboBoxAttachment> seqRateAttachment, seqModeAttachment;

    std::unique_ptr<juce::ParameterAttachment> filterEnabledListener;   // last: destroyed first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicSynthEditor)
};

inline juce::AudioProcessorEditor* BasicSynthAudioProcessor::createEditor()
{
    return new BasicSynthEditor (*this);
}
