// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "Visualisers.h"

/**
    Quick-reference help for Neon Synth, shown over the whole editor when the
    "?" button is pressed.

    The text is laid out on the editor's 760 x 520 design grid and drawn under
    the same uniform scale as everything else, so it stays legible at any
    window size without re-flowing. It is plain static text, built once.

    Closing: click anywhere on the overlay or press Escape. The editor's "?"
    button also toggles it. The overlay takes all mouse clicks while it is
    visible, so nothing underneath is changed by accident.

    To edit the help, change the entries in buildColumns() - each one is a
    heading followed by its lines.
*/
class HelpOverlay : public juce::Component
{
public:
    /** Called when the overlay closes itself (click or Escape), so the owner can update its button. */
    std::function<void()> onClose;

    explicit HelpOverlay (int designWidth) : baseWidth (designWidth)
    {
        buildColumns();
        setWantsKeyboardFocus (true);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.82f));

        const float s = (float) getWidth() / (float) baseWidth;
        g.addTransform (juce::AffineTransform::scale (s));

        neon::drawPanel (g, { 20.0f, 16.0f, (float) baseWidth - 40.0f, 594.0f }, neon::cyan);

        g.setColour (juce::Colours::white);
        g.setFont (neon::font (18.0f));
        g.drawText ("NEON SYNTH - QUICK HELP", 36, 26, 400, 26, juce::Justification::left);

        g.setColour (neon::cyan.withAlpha (0.6f));
        g.setFont (neon::font (11.0f));
        g.drawText ("Click anywhere or press Esc to close", baseWidth - 336, 30, 300, 20,
                    juce::Justification::right);

        for (size_t i = 0; i < columns.size(); ++i)
        {
            juce::TextLayout layout;
            layout.createLayout (columns[i], columnWidth);
            layout.draw (g, { columnX[i], 62.0f, columnWidth, 542.0f });
        }
    }

    void mouseDown (const juce::MouseEvent&) override  { close(); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            close();
            return true;
        }
        return false;
    }

    /** Shows the overlay and takes keyboard focus so Escape works straight away. */
    void show()
    {
        setVisible (true);
        toFront (true);
        grabKeyboardFocus();
    }

private:
    void close()
    {
        setVisible (false);
        if (onClose != nullptr)
            onClose();
    }

    using Section = std::pair<const char*, std::initializer_list<const char*>>;

    /** Appends a heading and its lines to one column. */
    static void addSection (juce::AttributedString& text, const Section& section, juce::Colour colour)
    {
        text.append (juce::String (section.first) + "\n", neon::font (12.5f, true), colour);
        for (const auto* line : section.second)
            text.append (juce::String (line) + "\n", neon::font (11.5f), juce::Colours::white.withAlpha (0.85f));
        text.append ("\n", neon::font (7.0f), colour);   // gap before the next heading
    }

    void buildColumns()
    {
        for (auto& c : columns)
        {
            c.setWordWrap (juce::AttributedString::byWord);
            c.setLineSpacing (1.5f);
        }

        // Left column: the step sequencer.
        addSection (columns[0], { "STEP SEQUENCER", {
            "RUN starts and stops the pattern.",
            "SYNC follows your DAW's tempo and transport: press play in the DAW to hear it. In the Standalone, or with SYNC off, TEMPO sets the speed.",
            "Rate is the step length: 1/16 = four steps per beat, T = triplets.",
            "SWING delays every second step. GATE sets how long each note lasts." } }, neon::amber);

        addSection (columns[0], { "EDITING STEPS", {
            "Click: switch a step on or off (its note is kept).",
            "Drag up / down: change the note (one semitone per 6 px).",
            "Mouse wheel: nudge the note by a semitone.",
            "Shift-drag or right-drag: velocity, shown by the bar at the bottom. Shift + wheel nudges it." } }, neon::amber);

        addSection (columns[0], { "MODES", {
            "Layer: the pattern plays along with your keyboard.",
            "Replace: the pattern only; keyboard notes are ignored.",
            "Transpose: the pattern plays while you hold a key, shifted by that key's distance from C4 (hold C5 for an octave up)." } }, neon::amber);

        // Right column: the synth and getting sound out.
        addSection (columns[1], { "SOUND", {
            "SOURCE: the oscillator (with its Waveform), the loaded sample, or Granular: a cloud of short grains of it.",
            "ENVELOPE: attack, decay, sustain and release of every note.",
            "FILTER: low-pass cutoff; ON switches it in.",
            "GAIN: output level. WIDTH: stereo spread of the voices.",
            "VOICES lights the 16 voices in use. SCOPE / SPECTRUM switch the display." } }, neon::cyan);

        addSection (columns[1], { "SAMPLE", {
            "LOAD a WAV, AIFF or FLAC file, or drop one anywhere on the synth. PAD goes back to the built-in pad.",
            "ROOT is the key that plays the file at its own pitch. Drag the cyan line to set the start and the amber brackets to set the loop.",
            "One-shot plays to the end; Loop loops while the note sounds; Loop until release loops while the key is held. XFADE smooths the loop point." } }, neon::cyan);

        addSection (columns[1], { "GRANULAR", {
            "POS places the cloud in the sample (or drag on the waveform); SCAN moves it, 0 freezes it. SIZE and DENS set grain length and grains per second.",
            "SPRAY scatters the grains around POS; JITTER detunes each one. ROOT works as for Sample." } }, neon::magenta);

        addSection (columns[1], { "PLAYING NOTES", {
            "MIDI keyboard in the Standalone: Options > Audio/MIDI Settings, then tick it under MIDI inputs.",
            "No keyboard? Press RUN, or use a virtual keyboard such as VMPK with loopMIDI." } }, neon::cyan);

        addSection (columns[1], { "NO SOUND?", {
            "Laptop speakers play very little below about 200 Hz. For low notes pick Sawtooth or Square, raise the notes, or use headphones.",
            "Check the output device and its active channels in Options > Audio/MIDI Settings." } }, neon::cyan);

        addSection (columns[1], { "SAVING", {
            "The pattern, the sample (up to 10 MB, otherwise by its path) and all settings are saved with your DAW project (and by the Standalone between runs)." } }, neon::cyan);
    }

    const int baseWidth;
    static constexpr float columnWidth = 330.0f;
    static constexpr float columnX[2] = { 36.0f, 394.0f };
    std::array<juce::AttributedString, 2> columns;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HelpOverlay)
};
