// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "NeonSynthProcessor.h"
#include "Visualisers.h"
#include "SequencerView.h"
#include "HelpOverlay.h"
#include "SampleView.h"
#include "ShapeView.h"

/**
    Neon Synth editor.

    Laid out on a fixed 760 x 520 design grid and scaled uniformly, so the
    window can be resized (fixed aspect ratio) without any layout logic beyond
    one scale factor.

    Left column: source and waveform, envelope (shape + knobs), filter, voice lights.
    Right column: scope / spectrum, stereo meter, output gain and stereo width.
    Sample strip: the loaded sample (load, built-in pad, loop mode), its
    waveform with draggable start and loop markers and live playheads, and
    the root / start / loop / crossfade knobs. A file can also be dropped
    anywhere on the editor.
    Bottom strip: the step sequencer - run / sync / tempo / rate / mode on the
    first row, the 16-step grid, then swing and gate.

    The "?" button in the top-right corner opens HelpOverlay, a quick-reference
    page drawn over the whole editor.

    A 30 Hz timer drains the processor's lock-free scope FIFO and reads the
    per-voice atomics; it is the only path from audio to GUI. Each view repaints
    only when what it shows has changed, so an idle editor costs almost nothing.
*/
class NeonSynthEditor : public juce::AudioProcessorEditor,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer
{
public:
    explicit NeonSynthEditor (NeonSynthProcessor& p)
        : AudioProcessorEditor (&p), processor (p), apvts (p.getAPVTS()), sampleView (p, p.getAPVTS()), shapeView (p, p.getAPVTS()), stepGrid (p)
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
        smpRootValue     = apvts.getRawParameterValue ("smpRoot");
        smpStartValue    = apvts.getRawParameterValue ("smpStart");
        smpLoopStartValue = apvts.getRawParameterValue ("smpLoopStart");
        smpLoopEndValue  = apvts.getRawParameterValue ("smpLoopEnd");
        smpXfadeValue    = apvts.getRawParameterValue ("smpXfade");
        grPosValue       = apvts.getRawParameterValue ("grPos");
        grScanValue      = apvts.getRawParameterValue ("grScan");
        grSizeValue      = apvts.getRawParameterValue ("grSize");
        grDensityValue   = apvts.getRawParameterValue ("grDensity");
        grSprayValue     = apvts.getRawParameterValue ("grSpray");
        grJitterValue    = apvts.getRawParameterValue ("grJitter");
        chopOvertoneValue = apvts.getRawParameterValue ("chopOvertone");
        chopCombValue     = apvts.getRawParameterValue ("chopComb");
        noiseValue        = apvts.getRawParameterValue ("noiseLevel");
        foldDriveValue    = apvts.getRawParameterValue ("foldDrive");
        foldMixValue      = apvts.getRawParameterValue ("foldMix");

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

        addAndMakeVisible (sourceCombo);
        sourceCombo.addItemList ({ "Oscillator", "Sample", "Granular", "Random" }, 1);   // same order as the parameter
        sourceCombo.setTooltip ("What the voices play: the oscillator (Waveform), the loaded sample, grains of it, or Random: a pitched wave of 16 random steps that keeps evolving");
        for (auto id : { juce::ComboBox::outlineColourId, juce::ComboBox::textColourId, juce::ComboBox::arrowColourId })
            sourceCombo.setColour (id, neon::cyan);
        sourceAttachment = std::make_unique<ComboBoxAttachment> (apvts, "source", sourceCombo);

        // ── Sample ───────────────────────────────────────────────────────────
        addAndMakeVisible (sampleView);
        addChildComponent (shapeView);   // in the sample view's place while Oscillator or Random plays
        setUpReadout (sampleNameLabel, neon::cyan, juce::Justification::centredLeft);
        sampleNameLabel.setInterceptsMouseClicks (true, false);   // so its tooltip (the full path) shows
        setUpReadout (sampleStatusLabel, neon::red, juce::Justification::centredRight);

        for (auto* button : { &loadButton, &padButton })
        {
            button->setColour (juce::TextButton::buttonColourId,  juce::Colours::transparentBlack);
            button->setColour (juce::TextButton::textColourOffId, neon::cyan);
            button->setColour (juce::ComboBox::outlineColourId,   neon::cyan.withAlpha (0.6f));
            addAndMakeVisible (button);
        }
        loadButton.setTooltip ("Load a WAV, AIFF or FLAC file (or drop one on the synth)");
        padButton.setTooltip ("Go back to the built-in pad");
        loadButton.onClick = [this] { chooseSampleFile(); };
        padButton.onClick  = [this] { processor.useDefaultSample(); };

        addAndMakeVisible (smpLoopCombo);
        smpLoopCombo.addItemList ({ "One-shot", "Loop", "Loop until release" }, 1);   // same order as the parameter
        smpLoopCombo.setTooltip ("One-shot: play to the end.  Loop: loop while the note sounds.  "
                                 "Loop until release: loop while the key is held, then play on to the end.");
        for (auto id : { juce::ComboBox::outlineColourId, juce::ComboBox::textColourId, juce::ComboBox::arrowColourId })
            smpLoopCombo.setColour (id, neon::cyan);
        smpLoopAttachment = std::make_unique<ComboBoxAttachment> (apvts, "smpLoop", smpLoopCombo);

        const std::pair<juce::Slider*, const char*> sampleKnobs[] = {
            { &smpRootKnob, "smpRoot" }, { &smpStartKnob, "smpStart" }, { &smpLoopStartKnob, "smpLoopStart" },
            { &smpLoopEndKnob, "smpLoopEnd" }, { &smpXfadeKnob, "smpXfade" } };
        for (size_t i = 0; i < std::size (sampleKnobs); ++i)
        {
            auto* knob = sampleKnobs[i].first;
            knob->setSliderStyle (juce::Slider::Rotary);
            knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::cyan);
            knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::cyan.withAlpha (0.3f));
            knob->setColour (juce::Slider::thumbColourId,               neon::cyan);
            addAndMakeVisible (knob);
            sampleKnobAttachments[i] = std::make_unique<SliderAttachment> (apvts, sampleKnobs[i].second, *knob);
        }
        smpRootKnob.setTooltip ("Root: the key at which the sample plays at its own pitch");
        smpXfadeKnob.setTooltip ("Loop crossfade length");

        const std::pair<juce::Slider*, const char*> grainKnobs[] = {
            { &grPosKnob, "grPos" }, { &grScanKnob, "grScan" }, { &grSizeKnob, "grSize" },
            { &grDensityKnob, "grDensity" }, { &grSprayKnob, "grSpray" }, { &grJitterKnob, "grJitter" } };
        for (size_t i = 0; i < std::size (grainKnobs); ++i)
        {
            auto* knob = grainKnobs[i].first;
            knob->setSliderStyle (juce::Slider::Rotary);
            knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::magenta);
            knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::magenta.withAlpha (0.3f));
            knob->setColour (juce::Slider::thumbColourId,               neon::magenta);
            addChildComponent (knob);
            grainKnobAttachments[i] = std::make_unique<SliderAttachment> (apvts, grainKnobs[i].second, *knob);
        }
        grPosKnob.setTooltip ("Position: where in the sample the cloud sits (or drag the magenta line on the waveform)");
        grScanKnob.setTooltip ("Scan: how fast the cloud moves through the sample, in either direction; 0 freezes it");
        grSizeKnob.setTooltip ("Size: length of each grain");
        grDensityKnob.setTooltip ("Density: grains started per second");
        grSprayKnob.setTooltip ("Spray: how far each grain may start from the cloud's position");
        grJitterKnob.setTooltip ("Pitch jitter: random detune of each grain, in cents");

        // Waveform sculpting (v1.6.0): the knob row while Oscillator or Random plays.
        const std::pair<juce::Slider*, const char*> shapeKnobs[] = {
            { &chopOvertoneKnob, "chopOvertone" }, { &chopCombKnob, "chopComb" }, { &noiseKnob, "noiseLevel" },
            { &foldDriveKnob, "foldDrive" }, { &foldMixKnob, "foldMix" } };
        for (size_t i = 0; i < std::size (shapeKnobs); ++i)
        {
            auto* knob = shapeKnobs[i].first;
            knob->setSliderStyle (juce::Slider::Rotary);
            knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::cyan);
            knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::cyan.withAlpha (0.3f));
            knob->setColour (juce::Slider::thumbColourId,               neon::cyan);
            addChildComponent (knob);
            shapeKnobAttachments[i] = std::make_unique<SliderAttachment> (apvts, shapeKnobs[i].second, *knob);
        }
        chopOvertoneKnob.setTooltip ("Overtone: how far CHOP cuts the shaded steps. 0% leaves them, 100% silences them, 200% turns them upside down");
        chopCombKnob.setTooltip ("Comb: repeats the 16-step pattern this many times per cycle, for metallic tones");
        noiseKnob.setTooltip ("Noise: crossfades the wave towards white noise, keeping the level steady");
        foldDriveKnob.setTooltip ("Drive: how hard the wavefolder folds the wave back on itself");
        foldMixKnob.setTooltip ("Fold: how much of the folded wave to hear. 0% switches the folder off");
        for (auto* label : { &chopOvertoneReadout, &chopCombReadout, &noiseReadout, &foldDriveReadout, &foldMixReadout })
            setUpReadout (*label, neon::cyan, juce::Justification::centred);

        for (auto* label : { &smpRootReadout, &smpStartReadout, &smpLoopStartReadout, &smpLoopEndReadout, &smpXfadeReadout,
                             &grPosReadout, &grScanReadout, &grSizeReadout, &grDensityReadout, &grSprayReadout, &grJitterReadout })
            setUpReadout (*label, neon::cyan, juce::Justification::centred);
        for (auto* label : { &grPosReadout, &grScanReadout, &grSizeReadout, &grDensityReadout, &grSprayReadout, &grJitterReadout })
            label->setColour (juce::Label::textColourId, neon::magenta);

        // Waveform only applies to the oscillator, the loop mode only to Sample, and the strip
        // follows the source: Sample's knobs, Granular's, or (Oscillator and Random) the shape view
        // with the sculpting knobs.
        sourceListener = std::make_unique<juce::ParameterAttachment> (
            *apvts.getParameter ("source"),
            [this] (float value)
            {
                const int source = juce::roundToInt (value);
                const auto mode = source == 1 ? StripMode::sample : source == 2 ? StripMode::granular : StripMode::shape;
                waveformCombo.setAlpha (source == 0 ? 1.0f : 0.45f);
                smpLoopCombo.setAlpha (mode == StripMode::sample ? 1.0f : 0.45f);
                sampleNameLabel.setAlpha (mode == StripMode::shape ? 0.45f : 1.0f);
                sampleView.setVisible (mode != StripMode::shape);
                shapeView.setVisible (mode == StripMode::shape);
                if (std::exchange (stripMode, mode) != stripMode && getWidth() > 0)
                {
                    resized();
                    repaint();
                }
            });
        sourceListener->sendInitialUpdate();

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

        // Help: "?" toggles the overlay; the overlay un-toggles the button when it closes itself.
        helpButton.setClickingTogglesState (true);
        helpButton.setTooltip ("Quick help");
        helpButton.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
        helpButton.setColour (juce::TextButton::buttonOnColourId, neon::cyan.withAlpha (0.25f));
        helpButton.setColour (juce::TextButton::textColourOffId,  neon::cyan);
        helpButton.setColour (juce::TextButton::textColourOnId,   neon::cyan);
        helpButton.setColour (juce::ComboBox::outlineColourId,    neon::cyan.withAlpha (0.6f));
        helpButton.onClick = [this]
        {
            if (helpButton.getToggleState()) helpOverlay.show();
            else                             helpOverlay.setVisible (false);
        };
        helpOverlay.onClose = [this] { helpButton.setToggleState (false, juce::dontSendNotification); };
        addAndMakeVisible (helpButton);
        addChildComponent (helpOverlay);   // added last, so it sits above every control

        // The chosen view is GUI state, saved with the plugin's state but not automatable.
        setView ((int) apvts.state.getProperty (viewPropertyId, 0));

        setResizable (true, true);
        setResizeLimits (baseWidth * 3 / 4, baseHeight * 3 / 4, baseWidth * 2, baseHeight * 2);
        getConstrainer()->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
        setSize (baseWidth, baseHeight);

        // While the editor was closed the scope FIFO filled up with old audio;
        // throw it away so the first frame shows what is playing now.
        while (processor.drainScope (scratchLeft.data(), scratchRight.data(), (int) scratchLeft.size()) > 0) {}

        updateReadouts();
        startTimerHz (timerHz);
    }

    ~NeonSynthEditor() override
    {
        stopTimer();   // the Timer base outlives our members; make sure no tick can arrive mid-teardown
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
        place (sourceCombo,   30, 72, 140, 28);
        place (waveformCombo, 178, 72, 192, 28);
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

        // Sample strip
        place (sampleNameLabel,   100, 403, 196, 16);
        place (loadButton,        300, 400, 58, 22);
        place (padButton,         362, 400, 58, 22);
        place (smpLoopCombo,      430, 400, 150, 22);
        place (sampleStatusLabel, 586, 403, 154, 16);
        place (sampleView,        30, 428, 390, 62);
        place (shapeView,         30, 428, 390, 62);

        for (const auto& slot : allKnobSlots())
        {
            slot.knob->setVisible (false);
            slot.readout->setVisible (false);
        }
        const auto row = knobRow();
        for (size_t i = 0; i < row.size(); ++i)
        {
            const auto x = (float) rowKnobX ((int) i, (int) row.size());
            const auto w = (float) knobWidth ((int) row.size());
            place (*row[i].knob,    x, 426, w, 38);
            place (*row[i].readout, x, 478, w, 12);
            row[i].knob->setVisible (true);
            row[i].readout->setVisible (true);
        }

        // Sequencer strip
        place (seqOnButton,     135, 506, 62, 22);
        place (seqSyncButton,   200, 506, 66, 22);
        place (seqTempoSlider,  316, 506, 150, 22);
        place (seqTempoReadout, 466, 509, 84, 16);
        place (seqRateCombo,    560, 506, 70, 22);
        place (seqModeCombo,    636, 506, 104, 22);
        place (stepGrid,        30, 536, 710, 50);
        place (seqSwingSlider,  80, 592, 230, 22);
        place (seqSwingReadout, 310, 595, 60, 16);
        place (seqGateSlider,   446, 592, 234, 22);
        place (seqGateReadout,  680, 595, 60, 16);

        place (helpButton, 712, 12, 28, 24);
        helpOverlay.setBounds (getLocalBounds());

        for (auto* label : { &attackReadout, &decayReadout, &sustainReadout, &releaseReadout,
                             &cutoffReadout, &gainReadout, &widthReadout,
                             &seqTempoReadout, &seqSwingReadout, &seqGateReadout, &sampleNameLabel, &sampleStatusLabel,
                             &smpRootReadout, &smpStartReadout, &smpLoopStartReadout, &smpLoopEndReadout, &smpXfadeReadout,
                             &grPosReadout, &grScanReadout, &grSizeReadout, &grDensityReadout, &grSprayReadout, &grJitterReadout,
                             &chopOvertoneReadout, &chopCombReadout, &noiseReadout, &foldDriveReadout, &foldMixReadout })
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

        // Sample strip
        g.setColour (neon::cyan.withAlpha (0.25f));
        g.drawHorizontalLine (394, 30.0f, 740.0f);
        g.setColour (neon::cyan);
        g.setFont (neon::font (13.0f));
        g.drawText (stripMode == StripMode::shape ? "SHAPE" : "SAMPLE", 30, 401, 70, 20, juce::Justification::left);
        g.setFont (neon::font (11.0f));
        const auto row = knobRow();
        for (size_t i = 0; i < row.size(); ++i)
        {
            g.setColour (row[i].knob == &smpRootKnob || stripMode != StripMode::granular ? neon::cyan : neon::magenta);
            g.drawText (row[i].name, rowKnobX ((int) i, (int) row.size()), 465, knobWidth ((int) row.size()), 13, juce::Justification::centred);
        }

        // Sequencer strip
        g.setColour (neon::amber.withAlpha (0.25f));
        g.drawHorizontalLine (500, 30.0f, 740.0f);
        g.setColour (neon::amber);
        g.setFont (neon::font (13.0f));
        g.drawText ("SEQUENCER", 30, 507, 100, 20, juce::Justification::left);
        g.setFont (neon::font (11.0f));
        g.drawText ("TEMPO", 272, 507, 44, 20, juce::Justification::left);
        g.drawText ("SWING", 30,  593, 50, 20, juce::Justification::left);
        g.drawText ("GATE",  400, 593, 46, 20, juce::Justification::left);

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
    static constexpr int baseHeight = 626;
    static constexpr int timerHz    = 30;
    static inline const juce::Identifier viewPropertyId { "view" };

    float scale() const noexcept { return (float) getWidth() / (float) baseWidth; }

    // The knob row of the sample strip: Sample's five knobs, Granular's seven (sharing ROOT),
    // or the five sculpting knobs while Oscillator or Random plays.
    struct KnobSlot { juce::Slider* knob; juce::Label* readout; const char* name; };
    enum class StripMode { sample, granular, shape };

    std::vector<KnobSlot> knobRow() { return knobRowFor (stripMode); }

    std::vector<KnobSlot> knobRowFor (StripMode mode)
    {
        if (mode == StripMode::shape)
            return { { &chopOvertoneKnob, &chopOvertoneReadout, "OVERTONE" }, { &chopCombKnob, &chopCombReadout, "COMB" },
                     { &noiseKnob, &noiseReadout, "NOISE" }, { &foldDriveKnob, &foldDriveReadout, "DRIVE" },
                     { &foldMixKnob, &foldMixReadout, "FOLD" } };

        if (mode == StripMode::granular)
            return { { &smpRootKnob, &smpRootReadout, "ROOT" }, { &grPosKnob, &grPosReadout, "POS" },
                     { &grScanKnob, &grScanReadout, "SCAN" }, { &grSizeKnob, &grSizeReadout, "SIZE" },
                     { &grDensityKnob, &grDensityReadout, "DENS" }, { &grSprayKnob, &grSprayReadout, "SPRAY" },
                     { &grJitterKnob, &grJitterReadout, "JITTER" } };

        return { { &smpRootKnob, &smpRootReadout, "ROOT" }, { &smpStartKnob, &smpStartReadout, "START" },
                 { &smpLoopStartKnob, &smpLoopStartReadout, "L.START" }, { &smpLoopEndKnob, &smpLoopEndReadout, "L.END" },
                 { &smpXfadeKnob, &smpXfadeReadout, "XFADE" } };
    }

    std::vector<KnobSlot> allKnobSlots()
    {
        std::vector<KnobSlot> all;
        for (const auto mode : { StripMode::sample, StripMode::granular, StripMode::shape })
        {
            const auto row = knobRowFor (mode);
            all.insert (all.end(), row.begin(), row.end());
        }
        return all;
    }

    /** 5 knobs at 62 apart, or 7 at 44 apart, from x = 430 to 740. */
    static int knobSpacing (int count) noexcept   { return count > 5 ? 44 : 62; }
    static int knobWidth (int count) noexcept     { return knobSpacing (count) - 4; }
    static int rowKnobX (int i, int count) noexcept { return 430 + knobSpacing (count) * i; }

    /** name + details, with the name shortened ("...") so the details always fit the label. */
    juce::String fitSampleName (const juce::String& name, const juce::String& details) const
    {
        const auto font = sampleNameLabel.getFont();
        const auto width = [&font] (const juce::String& text)
        {
            juce::GlyphArrangement glyphs;
            glyphs.addLineOfText (font, text, 0.0f, 0.0f);
            return glyphs.getBoundingBox (0, -1, true).getWidth();
        };

        const float available = (float) sampleNameLabel.getWidth() - 4.0f;
        if (width (name + details) <= available)
            return name + details;

        auto shortened = name;
        while (shortened.length() > 1 && width (shortened + "..." + details) > available)
            shortened = shortened.dropLastCharacters (1);
        return shortened.trimEnd() + "..." + details;
    }

    /** 48000 -> "48 kHz", 44100 -> "44.1 kHz", 22050 -> "22.05 kHz". */
    static juce::String formatSampleRate (double rate)
    {
        if (rate <= 0.0)
            return {};
        const auto hz = juce::roundToInt (rate);
        const int decimals = hz % 1000 == 0 ? 0 : (hz % 100 == 0 ? 1 : 2);
        return juce::String (hz / 1000.0, decimals) + " kHz";
    }

    //==========================================================================
    // Sample files

    static bool isSampleFile (const juce::String& path)
    {
        return juce::File (path).hasFileExtension ("wav;aif;aiff;flac");
    }

    void chooseSampleFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (const auto file = fc.getResult(); file.existsAsFile())
                                      processor.loadSampleFileAsync (file);
                              });
    }

public:
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return files.size() == 1 && isSampleFile (files[0]);
    }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        if (files.size() == 1 && isSampleFile (files[0]))
            processor.loadSampleFileAsync (juce::File (files[0]));
    }

private:

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

        const auto percent = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
        set (smpRootReadout,      juce::MidiMessage::getMidiNoteName (juce::roundToInt (smpRootValue->load()), true, true, 4));
        set (smpStartReadout,     percent (smpStartValue->load()));
        set (smpLoopStartReadout, percent (smpLoopStartValue->load()));
        set (smpLoopEndReadout,   percent (smpLoopEndValue->load()));
        set (smpXfadeReadout,     juce::String (juce::roundToInt (smpXfadeValue->load())) + " ms");

        const float scan = grScanValue->load();
        set (grPosReadout,     percent (grPosValue->load()));
        set (grScanReadout,    scan == 0.0f ? juce::String ("frozen") : (scan > 0.0f ? "+" : "") + juce::String (scan, 2) + "x");
        set (grSizeReadout,    juce::String (juce::roundToInt (grSizeValue->load())) + " ms");
        set (grDensityReadout, juce::String (juce::roundToInt (grDensityValue->load())) + "/s");
        set (grSprayReadout,   percent (grSprayValue->load()));
        set (grJitterReadout,  juce::String (juce::roundToInt (grJitterValue->load())) + " ct");

        set (chopOvertoneReadout, juce::String (juce::roundToInt (chopOvertoneValue->load())) + "%");
        set (chopCombReadout,     "x" + juce::String (juce::roundToInt (chopCombValue->load())));
        set (noiseReadout,        juce::String (juce::roundToInt (noiseValue->load())) + "%");
        set (foldDriveReadout,    juce::String (foldDriveValue->load(), 1));
        set (foldMixReadout,      juce::String (juce::roundToInt (foldMixValue->load())) + "%");

        const auto status = processor.getSampleStatus();
        // Re-fitted only when the text or the label's size changes (it measures glyphs).
        const auto details = "  " + juce::String (status.seconds, 1) + " s  " + formatSampleRate (status.sampleRate);
        if (status.name + details != fittedFor || sampleNameLabel.getWidth() != fittedWidth)
        {
            fittedFor   = status.name + details;
            fittedWidth = sampleNameLabel.getWidth();
            set (sampleNameLabel, fitSampleName (status.name, details));
        }
        sampleNameLabel.setTooltip (status.path.isNotEmpty() ? status.path : status.name);
        set (sampleStatusLabel, status.loading ? juce::String ("loading...") : status.problem);
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
        sampleView.refresh(); // sample, markers, playheads
        shapeView.refresh();  // the sculpted cycle, CHOP's steps
        updateReadouts();
    }

    NeonSynthProcessor&           processor;
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
    std::atomic<float>* smpRootValue = nullptr;
    std::atomic<float>* smpStartValue = nullptr;
    std::atomic<float>* smpLoopStartValue = nullptr;
    std::atomic<float>* smpLoopEndValue = nullptr;
    std::atomic<float>* smpXfadeValue = nullptr;
    std::atomic<float>* grPosValue = nullptr;
    std::atomic<float>* grScanValue = nullptr;
    std::atomic<float>* grSizeValue = nullptr;
    std::atomic<float>* grDensityValue = nullptr;
    std::atomic<float>* grSprayValue = nullptr;
    std::atomic<float>* grJitterValue = nullptr;
    std::atomic<float>* chopOvertoneValue = nullptr;
    std::atomic<float>* chopCombValue = nullptr;
    std::atomic<float>* noiseValue = nullptr;
    std::atomic<float>* foldDriveValue = nullptr;
    std::atomic<float>* foldMixValue = nullptr;

    // Visualisers and their data
    SignalHistory    history;
    OscilloscopeView oscilloscope;
    SpectrumView     spectrum;
    LevelMeter       meter;
    AdsrView         adsrView;
    VoiceLeds        voiceLeds;
    std::array<float, NeonSynthProcessor::scopeFifoSize> scratchLeft {}, scratchRight {}, scratchMid {};

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

    // Source and sample
    juce::ComboBox sourceCombo, smpLoopCombo;
    std::unique_ptr<ComboBoxAttachment> sourceAttachment, smpLoopAttachment;
    SampleView       sampleView;
    juce::Label      sampleNameLabel, sampleStatusLabel;
    juce::TextButton loadButton { "LOAD" }, padButton { "PAD" };
    juce::Slider     smpRootKnob, smpStartKnob, smpLoopStartKnob, smpLoopEndKnob, smpXfadeKnob;
    juce::Label      smpRootReadout, smpStartReadout, smpLoopStartReadout, smpLoopEndReadout, smpXfadeReadout;
    std::array<std::unique_ptr<SliderAttachment>, 5> sampleKnobAttachments;
    juce::Slider     grPosKnob, grScanKnob, grSizeKnob, grDensityKnob, grSprayKnob, grJitterKnob;
    juce::Label      grPosReadout, grScanReadout, grSizeReadout, grDensityReadout, grSprayReadout, grJitterReadout;
    std::array<std::unique_ptr<SliderAttachment>, 6> grainKnobAttachments;
    ShapeView        shapeView;
    juce::Slider     chopOvertoneKnob, chopCombKnob, noiseKnob, foldDriveKnob, foldMixKnob;
    juce::Label      chopOvertoneReadout, chopCombReadout, noiseReadout, foldDriveReadout, foldMixReadout;
    std::array<std::unique_ptr<SliderAttachment>, 5> shapeKnobAttachments;
    StripMode stripMode = StripMode::shape;   // what the sample strip shows (follows Source)
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String fittedFor;      // the sample label's last unshortened text, and the width it was fitted to
    int fittedWidth = -1;

    // Step sequencer
    StepGrid           stepGrid;
    juce::TextButton seqOnButton { "RUN" }, seqSyncButton { "SYNC" };
    juce::Slider       seqTempoSlider, seqSwingSlider, seqGateSlider;
    juce::ComboBox     seqRateCombo, seqModeCombo;
    juce::Label        seqTempoReadout, seqSwingReadout, seqGateReadout;
    std::unique_ptr<ButtonAttachment>   seqOnAttachment, seqSyncAttachment;
    std::unique_ptr<SliderAttachment>   seqTempoAttachment, seqSwingAttachment, seqGateAttachment;
    std::unique_ptr<ComboBoxAttachment> seqRateAttachment, seqModeAttachment;

    // Help
    juce::TextButton helpButton { "?" };
    HelpOverlay      helpOverlay { baseWidth };

    std::unique_ptr<juce::ParameterAttachment> sourceListener;
    std::unique_ptr<juce::ParameterAttachment> filterEnabledListener;   // last: destroyed first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeonSynthEditor)
};

inline juce::AudioProcessorEditor* NeonSynthProcessor::createEditor()
{
    return new NeonSynthEditor (*this);
}
