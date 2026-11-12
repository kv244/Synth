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

    Laid out once on a fixed 1280 x 860 design grid. Every control lives in one
    child component ("content") of that size, and the editor scales that component
    to the window with a single transform (fixed aspect ratio, 75% to 150%), so the
    layout needs no scale arithmetic and a font size means pixels at 100%. The
    window opens at 100% if that fits the screen, otherwise at 75%.

    Header: name, voice lights, help.
    SOURCE panel: source and waveform, the sample controls (load, built-in pad, loop
    mode), the strip (sample waveform with draggable markers, or the sculpted
    cycle) and the knob row under it. A file can also be dropped anywhere.
    Middle: scope / spectrum, envelope (shape + knobs) and filter.
    OUTPUT panel: gain, stereo width and the stereo meter.
    SEQUENCER panel: run / sync / rate / mode / tempo / swing / gate, then the 16 steps.

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
        addAndMakeVisible (content);
        content.onPaint = [this] (juce::Graphics& g) { paintContent (g); };

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
        content.addAndMakeVisible (waveformCombo);
        waveformCombo.addItem ("Sine",     1);
        waveformCombo.addItem ("Square",   2);
        waveformCombo.addItem ("Triangle", 3);
        waveformCombo.addItem ("Sawtooth", 4);
        waveformCombo.setColour (juce::ComboBox::outlineColourId, neon::cyan);
        waveformCombo.setColour (juce::ComboBox::textColourId,    neon::cyan);
        waveformCombo.setColour (juce::ComboBox::arrowColourId,   neon::cyan);
        waveformAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
            (apvts, "waveform", waveformCombo);

        content.addAndMakeVisible (sourceCombo);
        sourceCombo.addItemList ({ "Oscillator", "Sample", "Granular", "Random" }, 1);   // same order as the parameter
        sourceCombo.setTooltip ("What the voices play: the oscillator (Waveform), the loaded sample, grains of it, or Random: a pitched wave of 16 random steps that keeps evolving");
        for (auto id : { juce::ComboBox::outlineColourId, juce::ComboBox::textColourId, juce::ComboBox::arrowColourId })
            sourceCombo.setColour (id, neon::cyan);
        sourceAttachment = std::make_unique<ComboBoxAttachment> (apvts, "source", sourceCombo);

        // ── Sample ───────────────────────────────────────────────────────────
        content.addAndMakeVisible (sampleView);
        content.addChildComponent (shapeView);   // in the sample view's place while Oscillator or Random plays
        setUpReadout (sampleNameLabel, neon::cyan, juce::Justification::centredLeft);
        sampleNameLabel.setInterceptsMouseClicks (true, false);   // so its tooltip (the full path) shows
        setUpReadout (sampleStatusLabel, neon::red, juce::Justification::centredRight);

        for (auto* button : { &loadButton, &padButton })
        {
            button->setColour (juce::TextButton::buttonColourId,  juce::Colours::transparentBlack);
            button->setColour (juce::TextButton::textColourOffId, neon::cyan);
            button->setColour (juce::ComboBox::outlineColourId,   neon::cyan.withAlpha (0.6f));
            content.addAndMakeVisible (button);
        }
        loadButton.setTooltip ("Load a WAV, AIFF or FLAC file (or drop one on the synth)");
        padButton.setTooltip ("Go back to the built-in pad");
        loadButton.onClick = [this] { chooseSampleFile(); };
        padButton.onClick  = [this] { processor.useDefaultSample(); };

        content.addAndMakeVisible (smpLoopCombo);
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
            content.addAndMakeVisible (knob);
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
            content.addChildComponent (knob);
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
            content.addChildComponent (knob);
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
                    layoutKnobRow();
                    content.repaint();
                }
            });
        sourceListener->sendInitialUpdate();

        // ── Envelope ─────────────────────────────────────────────────────────
        content.addAndMakeVisible (adsrView);
        adsrView.attach (apvts.getParameter ("attack"), apvts.getParameter ("decay"),
                         apvts.getParameter ("sustain"), apvts.getParameter ("release"));

        for (auto* knob : { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob })
        {
            knob->setSliderStyle (juce::Slider::Rotary);
            knob->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            knob->setColour (juce::Slider::rotarySliderFillColourId,    neon::amber);
            knob->setColour (juce::Slider::rotarySliderOutlineColourId, neon::amber.withAlpha (0.3f));
            knob->setColour (juce::Slider::thumbColourId,               neon::amber);
            content.addAndMakeVisible (knob);
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

        content.addAndMakeVisible (filterToggle);
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
        content.addAndMakeVisible (voiceLeds);

        // ── Output: display, meter, gain, width ──────────────────────────────
        content.addChildComponent (oscilloscope);
        content.addChildComponent (spectrum);
        content.addAndMakeVisible (meter);

        for (auto* button : { &scopeButton, &spectrumButton })
        {
            button->setClickingTogglesState (true);
            button->setRadioGroupId (1);
            button->setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
            button->setColour (juce::TextButton::buttonOnColourId, neon::cyan.withAlpha (0.25f));
            button->setColour (juce::TextButton::textColourOffId,  neon::cyan.withAlpha (0.55f));
            button->setColour (juce::TextButton::textColourOnId,   neon::cyan);
            button->setColour (juce::ComboBox::outlineColourId,    neon::cyan.withAlpha (0.5f));
            content.addAndMakeVisible (button);
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
            content.addAndMakeVisible (toggle);
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
            content.addAndMakeVisible (combo);
        }
        seqRateCombo.addItemList ({ "1/8", "1/16", "1/32", "1/8T", "1/16T" }, 1);   // same order as the parameter
        seqModeCombo.addItemList ({ "Layer", "Replace", "Transpose" }, 1);
        seqRateCombo.setTooltip ("Step length");
        seqModeCombo.setTooltip ("Layer: sequencer + keyboard.  Replace: keyboard ignored.  "
                                 "Transpose: the pattern plays while a key is held, shifted by (key - C4).");
        seqRateAttachment = std::make_unique<ComboBoxAttachment> (apvts, "seqRate", seqRateCombo);
        seqModeAttachment = std::make_unique<ComboBoxAttachment> (apvts, "seqMode", seqModeCombo);

        content.addAndMakeVisible (stepGrid);

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
        content.addAndMakeVisible (helpButton);
        content.addChildComponent (helpOverlay);   // added last, so it sits above every control

        // Component IDs: a control attached to a parameter carries that parameter's ID, and the
        // views and buttons carry the names below. EditorTests finds everything by these IDs
        // alone, so any layout that keeps them passes the same tests.
        const std::pair<juce::Component*, const char*> ids[] = {
            { &waveformCombo, "waveform" }, { &sourceCombo, "source" }, { &smpLoopCombo, "smpLoop" },
            { &smpRootKnob, "smpRoot" }, { &smpStartKnob, "smpStart" }, { &smpLoopStartKnob, "smpLoopStart" },
            { &smpLoopEndKnob, "smpLoopEnd" }, { &smpXfadeKnob, "smpXfade" },
            { &grPosKnob, "grPos" }, { &grScanKnob, "grScan" }, { &grSizeKnob, "grSize" },
            { &grDensityKnob, "grDensity" }, { &grSprayKnob, "grSpray" }, { &grJitterKnob, "grJitter" },
            { &chopOvertoneKnob, "chopOvertone" }, { &chopCombKnob, "chopComb" }, { &noiseKnob, "noiseLevel" },
            { &foldDriveKnob, "foldDrive" }, { &foldMixKnob, "foldMix" },
            { &attackKnob, "attack" }, { &decayKnob, "decay" }, { &sustainKnob, "sustain" }, { &releaseKnob, "release" },
            { &cutoffSlider, "cutoff" }, { &filterToggle, "filterEnabled" },
            { &gainSlider, "gain" }, { &widthSlider, "width" },
            { &seqOnButton, "seqOn" }, { &seqSyncButton, "seqSync" }, { &seqTempoSlider, "seqTempo" },
            { &seqRateCombo, "seqRate" }, { &seqModeCombo, "seqMode" }, { &seqSwingSlider, "seqSwing" },
            { &seqGateSlider, "seqGate" },
            { &sampleView, "sampleView" }, { &shapeView, "shapeView" }, { &stepGrid, "stepGrid" },
            { &adsrView, "adsrView" }, { &oscilloscope, "scopeView" }, { &spectrum, "spectrumView" },
            { &scopeButton, "scopeButton" }, { &spectrumButton, "spectrumButton" },
            { &loadButton, "loadButton" }, { &padButton, "padButton" },
            { &helpButton, "helpButton" }, { &helpOverlay, "helpOverlay" } };
        for (const auto& [component, id] : ids)
            component->setComponentID (id);

        // The chosen view is GUI state, saved with the plugin's state but not automatable.
        setView ((int) apvts.state.getProperty (viewPropertyId, 0));

        layout();

        // 75% to 150%, fixed aspect ratio; opens at the largest of 75% and 100% that fits the screen.
        setResizable (true, true);
        setResizeLimits (baseWidth * 3 / 4, baseHeight * 3 / 4, baseWidth * 3 / 2, baseHeight * 3 / 2);
        getConstrainer()->setFixedAspectRatio ((double) baseWidth / (double) baseHeight);
        const float openScale = openingScale();
        setSize (juce::roundToInt ((float) baseWidth * openScale), juce::roundToInt ((float) baseHeight * openScale));

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
        // Everything is laid out in design units inside "content"; the window just scales it.
        content.setBounds (0, 0, baseWidth, baseHeight);
        content.setTransform (juce::AffineTransform::scale (scale()));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (neon::background);   // the strip around the content when the window is a pixel off the aspect ratio
    }

private:
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    static constexpr int baseWidth  = 1280;
    static constexpr int baseHeight = 860;
    static constexpr int timerHz    = 30;
    static inline const juce::Identifier viewPropertyId { "view" };

    float scale() const noexcept { return (float) getWidth() / (float) baseWidth; }

    /** 100% if the window (and a title bar's worth) fits the main screen's usable area, otherwise 75%. */
    static float openingScale()
    {
        constexpr int titleBar = 48;
        if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto area = display->userArea;
            if (area.getWidth() < baseWidth || area.getHeight() < baseHeight + titleBar)
                return 0.75f;
        }
        return 1.0f;
    }

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

    /** The knob row spans the SOURCE panel's inner width (x = 32 to 400), shared out evenly. */
    static juce::Range<int> rowKnobSpan (int i, int count) noexcept
    {
        const auto edge = [count] (int k) { return 32 + juce::roundToInt (368.0f * (float) k / (float) count); };
        return { edge (i), edge (i + 1) };
    }

    /** A panel, as the design draws it: dark fill, thin edge, rounded. */
    static void drawSection (juce::Graphics& g, juce::Rectangle<int> r)
    {
        g.setColour (neon::section);
        g.fillRoundedRectangle (r.toFloat(), 10.0f);
        g.setColour (neon::sectionEdge);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
    }

    void layoutKnobRow()
    {
        for (const auto& slot : allKnobSlots())
        {
            slot.knob->setVisible (false);
            slot.readout->setVisible (false);
        }
        const auto row = knobRow();
        for (size_t i = 0; i < row.size(); ++i)
        {
            const auto span = rowKnobSpan ((int) i, (int) row.size());
            row[i].knob->setBounds (span.getStart(), 516, span.getLength(), 52);
            row[i].readout->setBounds (span.getStart(), 588, span.getLength(), 14);
            row[i].knob->setVisible (true);
            row[i].readout->setVisible (true);
        }
    }

    /** Puts every control in its place on the 1280 x 860 grid (the knob row is placed by layoutKnobRow()). */
    void layout()
    {
        // Header
        voiceLeds.setBounds (880, 33, 288, 18);
        helpButton.setBounds (1212, 26, 40, 32);

        // SOURCE panel (16, 82, 400 x 548)
        sourceCombo.setBounds (32, 118, 176, 32);
        waveformCombo.setBounds (216, 118, 184, 32);
        loadButton.setBounds (32, 160, 64, 28);
        padButton.setBounds (102, 160, 56, 28);
        smpLoopCombo.setBounds (166, 160, 234, 28);
        sampleNameLabel.setBounds (32, 194, 258, 18);
        sampleStatusLabel.setBounds (294, 194, 106, 18);
        sampleView.setBounds (32, 218, 368, 290);
        shapeView.setBounds (32, 218, 368, 290);
        layoutKnobRow();

        // Middle: view (428, 82, 524 x 262), then envelope (428, 356, 330 x 274) and filter (770, 356, 182 x 274)
        scopeButton.setBounds (444, 92, 92, 28);
        spectrumButton.setBounds (536, 92, 110, 28);
        oscilloscope.setBounds (444, 128, 492, 204);
        spectrum.setBounds (444, 128, 492, 204);

        adsrView.setBounds (444, 392, 298, 116);
        juce::Slider* knobs[] = { &attackKnob, &decayKnob, &sustainKnob, &releaseKnob };
        juce::Label*  reads[] = { &attackReadout, &decayReadout, &sustainReadout, &releaseReadout };
        for (int i = 0; i < 4; ++i)
        {
            knobs[i]->setBounds (444 + 75 * i, 516, 70, 52);
            reads[i]->setBounds (444 + 75 * i, 588, 70, 14);
        }

        filterToggle.setBounds (872, 362, 64, 26);
        cutoffReadout.setBounds (856, 400, 80, 18);
        cutoffSlider.setBounds (786, 424, 150, 28);

        // OUTPUT panel (964, 82, 300 x 548)
        gainSlider.setBounds (1030, 126, 150, 28);
        gainReadout.setBounds (1184, 132, 64, 16);
        widthSlider.setBounds (1030, 160, 150, 28);
        widthReadout.setBounds (1184, 166, 64, 16);
        meter.setBounds (1040, 214, 160, 396);

        // SEQUENCER panel (16, 642, 1248 x 204)
        seqOnButton.setBounds (150, 652, 72, 28);
        seqSyncButton.setBounds (228, 652, 72, 28);
        seqRateCombo.setBounds (308, 652, 80, 28);
        seqModeCombo.setBounds (396, 652, 124, 28);
        seqTempoSlider.setBounds (602, 652, 116, 28);
        seqTempoReadout.setBounds (722, 656, 92, 20);
        seqSwingSlider.setBounds (884, 652, 100, 28);
        seqSwingReadout.setBounds (988, 656, 48, 20);
        seqGateSlider.setBounds (1100, 652, 92, 28);
        seqGateReadout.setBounds (1196, 656, 52, 20);
        stepGrid.setBounds (32, 692, 1216, 142);

        helpOverlay.setBounds (0, 0, baseWidth, baseHeight);

        // Every readout is 12 px or more at 100%.
        for (auto* label : { &attackReadout, &decayReadout, &sustainReadout, &releaseReadout,
                             &cutoffReadout, &gainReadout, &widthReadout,
                             &seqTempoReadout, &seqSwingReadout, &seqGateReadout, &sampleNameLabel, &sampleStatusLabel,
                             &smpRootReadout, &smpStartReadout, &smpLoopStartReadout, &smpLoopEndReadout, &smpXfadeReadout,
                             &grPosReadout, &grScanReadout, &grSizeReadout, &grDensityReadout, &grSprayReadout, &grJitterReadout,
                             &chopOvertoneReadout, &chopCombReadout, &noiseReadout, &foldDriveReadout, &foldMixReadout })
            label->setFont (neon::font (12.0f));
    }

    /** The panels and the static labels, drawn on the design grid (under the controls). */
    void paintContent (juce::Graphics& g)
    {
        g.fillAll (neon::background);
        for (const auto r : { juce::Rectangle<int> (16, 14, 1248, 56),    // header
                              juce::Rectangle<int> (16, 82, 400, 548),    // source
                              juce::Rectangle<int> (428, 82, 524, 262),   // view
                              juce::Rectangle<int> (428, 356, 330, 274),  // envelope
                              juce::Rectangle<int> (770, 356, 182, 274),  // filter
                              juce::Rectangle<int> (964, 82, 300, 548),   // output
                              juce::Rectangle<int> (16, 642, 1248, 204) }) // sequencer
            drawSection (g, r);

        // Header
        juce::AttributedString name;
        name.append ("NEON ", neon::font (22.0f, true), neon::text);
        name.append ("SYNTH", neon::font (22.0f, true), neon::cyan);
        name.draw (g, juce::Rectangle<float> (32.0f, 29.0f, 300.0f, 28.0f));
        g.setColour (neon::muted);
        g.setFont (neon::font (12.0f, true));
        g.drawText ("VOICES", 810, 33, 64, 18, juce::Justification::centredRight);

        const auto title = [&g] (const char* text, juce::Colour colour, int x, int y, int w = 150)
        {
            g.setColour (colour);
            g.setFont (neon::font (13.0f, true));
            g.drawText (text, x, y, w, 18, juce::Justification::left);
        };
        const auto label = [&g] (const char* text, juce::Colour colour, int x, int y, int w, int h, juce::Justification j = juce::Justification::left)
        {
            g.setColour (colour);
            g.setFont (neon::font (12.0f, true));
            g.drawText (text, x, y, w, h, j);
        };

        // SOURCE: the title, what the strip below shows, and the names of the knobs under it
        title ("SOURCE", neon::cyan, 32, 92);
        g.setColour (neon::muted);
        g.setFont (neon::font (12.0f));
        g.drawText (stripMode == StripMode::shape ? "SHAPE - one cycle" : stripMode == StripMode::sample ? "SAMPLE - drag the markers" : "GRAINS - drag POS",
                    152, 92, 248, 18, juce::Justification::right);
        const auto row = knobRow();
        for (size_t i = 0; i < row.size(); ++i)
        {
            const auto span = rowKnobSpan ((int) i, (int) row.size());
            label (row[i].name, row[i].knob == &smpRootKnob || stripMode != StripMode::granular ? neon::cyan : neon::magenta,
                   span.getStart(), 572, span.getLength(), 14, juce::Justification::centred);
        }

        // Envelope
        title ("ENVELOPE", neon::amber, 444, 366);
        const char* names[] = { "ATTACK", "DECAY", "SUSTAIN", "RELEASE" };
        for (int i = 0; i < 4; ++i)
            label (names[i], neon::amber, 444 + 75 * i, 572, 70, 14, juce::Justification::centred);

        // Filter
        title ("FILTER", neon::magenta, 786, 366, 80);
        label ("CUTOFF", neon::magenta, 786, 402, 66, 16);

        // Output
        title ("OUTPUT", neon::cyan, 980, 92);
        label ("GAIN",  neon::cyan, 980, 132, 48, 16);
        label ("WIDTH", neon::cyan, 980, 166, 48, 16);

        // Sequencer
        title ("SEQUENCER", neon::amber, 32, 657, 110);
        label ("TEMPO", neon::amber, 544, 656, 56, 20);
        label ("SWING", neon::amber, 830, 656, 52, 20);
        label ("GATE",  neon::amber, 1056, 656, 42, 20);
    }


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
        content.addAndMakeVisible (slider);
    }

    void setUpReadout (juce::Label& label, juce::Colour colour, juce::Justification justification)
    {
        label.setColour (juce::Label::textColourId, colour);
        label.setJustificationType (justification);
        label.setInterceptsMouseClicks (false, false);
        label.setBorderSize ({});
        content.addAndMakeVisible (label);
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

    /** Everything the editor shows lives here, at the design size; resized() scales it to the window. */
    struct Content : juce::Component
    {
        std::function<void (juce::Graphics&)> onPaint;
        void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
    };

    NeonSynthProcessor&           processor;
    juce::AudioProcessorValueTreeState& apvts;
    Content content;   // declared before every control, so it outlives them

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
    HelpOverlay      helpOverlay { baseWidth, baseHeight };

    std::unique_ptr<juce::ParameterAttachment> sourceListener;
    std::unique_ptr<juce::ParameterAttachment> filterEnabledListener;   // last: destroyed first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeonSynthEditor)
};

inline juce::AudioProcessorEditor* NeonSynthProcessor::createEditor()
{
    return new NeonSynthEditor (*this);
}
