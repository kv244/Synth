#pragma once
#include <JuceHeader.h>
#include "NeonSynthProcessor.h"
#include "Visualisers.h"

/**
    @file SequencerView.h

    The 16-step grid shown in the editor's sequencer strip.

    Each cell shows one step: lit when the step is on, the note name in the
    middle, and a velocity bar along the bottom. A cyan outline follows the
    playhead while the sequencer runs; every fourth cell has a brighter top
    edge to mark the beats.

    Editing (all on the message thread, written to the processor lock-free):
      - click                     toggle the step on / off
      - drag up / down            change the note, one semitone per 6 px
      - shift-drag or right-drag  change the velocity
      - mouse wheel               note +/- 1 semitone (with shift: velocity +/- 4)

    The grid keeps no pattern of its own: refresh(), called from the editor's
    timer, re-reads the 16 steps and the playhead from the processor and
    repaints only when something changed. That also keeps it in sync when a
    host loads a preset or a session.
*/
class StepGrid : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    explicit StepGrid (NeonSynthProcessor& p) : processor (p)
    {
        setTooltip ("Click: step on/off.  Drag up/down: note.  Shift- or right-drag: velocity.  Wheel: note (shift: velocity).");
        refresh();
    }

    /** Re-reads the pattern and playhead; repaints if anything changed. Message thread. */
    void refresh()
    {
        bool changed = false;
        for (int i = 0; i < StepSequencer::numSteps; ++i)
        {
            const auto s = processor.getSequencerStep (i);
            if (! (s == steps[(size_t) i])) { steps[(size_t) i] = s; changed = true; }
        }

        const int head = processor.getSequencerPlayhead();
        if (head != playhead) { playhead = head; changed = true; }

        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        for (int i = 0; i < StepSequencer::numSteps; ++i)
        {
            const auto  cell = cellBounds (i);
            const auto& s    = steps[(size_t) i];
            const bool  beat = i % 4 == 0;

            g.setColour (neon::panel);
            g.fillRoundedRectangle (cell, 3.0f);

            if (s.on)
            {
                g.setColour (neon::amber.withAlpha (0.10f + 0.25f * (float) s.velocity / 127.0f));
                g.fillRoundedRectangle (cell, 3.0f);
            }

            g.setColour (neon::amber.withAlpha (s.on ? 0.9f : 0.25f));
            g.drawRoundedRectangle (cell.reduced (0.5f), 3.0f, s.on ? 1.2f : 1.0f);

            if (beat)   // beat marker
            {
                g.setColour (neon::amber.withAlpha (s.on ? 1.0f : 0.5f));
                g.fillRect (cell.getX() + 3.0f, cell.getY(), cell.getWidth() - 6.0f, 2.0f);
            }

            // Note name
            g.setFont (neon::font (juce::jmin (12.0f, cell.getWidth() * 0.32f)));
            g.setColour (s.on ? juce::Colours::white : neon::amber.withAlpha (0.4f));
            g.drawText (juce::MidiMessage::getMidiNoteName (s.note, true, true, 4),
                        cell.withTrimmedBottom (6.0f), juce::Justification::centred);

            // Velocity bar
            const auto bar = cell.reduced (4.0f, 0.0f).removeFromBottom (7.0f).withTrimmedBottom (3.0f);
            g.setColour (neon::amber.withAlpha (s.on ? 0.85f : 0.25f));
            g.fillRect (bar.withWidth (bar.getWidth() * (float) s.velocity / 127.0f));

            // Playhead
            if (i == playhead)
            {
                g.setColour (neon::cyan.withAlpha (0.25f));
                g.drawRoundedRectangle (cell.expanded (1.5f), 4.0f, 4.0f);
                g.setColour (neon::cyan);
                g.drawRoundedRectangle (cell.reduced (0.5f), 3.0f, 1.6f);
            }
        }
    }

    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStep   = stepAt (e.position);
        dragged    = false;
        if (dragStep < 0) return;
        dragStart  = steps[(size_t) dragStep];
        dragStartY = e.position.y;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragStep < 0) return;

        const float dy = dragStartY - e.position.y;   // up = positive
        if (! dragged && std::abs (dy) < 3.0f)        // small wobble still counts as a click
            return;
        dragged = true;

        auto s = dragStart;
        if (e.mods.isShiftDown() || e.mods.isRightButtonDown())
            s.velocity = juce::jlimit (1, 127, dragStart.velocity + juce::roundToInt (dy));
        else
            s.note = juce::jlimit (0, 127, dragStart.note + juce::roundToInt (dy / 6.0f));

        write (dragStep, s);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragStep >= 0 && ! dragged)
        {
            auto s = steps[(size_t) dragStep];
            s.on = ! s.on;
            write (dragStep, s);
        }
        dragStep = -1;
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const int index = stepAt (e.position);
        if (index < 0 || wheel.deltaY == 0.0f) return;

        const int direction = wheel.deltaY > 0.0f ? 1 : -1;
        auto s = steps[(size_t) index];
        if (e.mods.isShiftDown()) s.velocity = juce::jlimit (1, 127, s.velocity + 4 * direction);
        else                      s.note     = juce::jlimit (0, 127, s.note + direction);
        write (index, s);
    }

private:
    juce::Rectangle<float> cellBounds (int index) const
    {
        const auto  bounds = getLocalBounds().toFloat();
        const float width  = bounds.getWidth() / (float) StepSequencer::numSteps;
        return { bounds.getX() + width * (float) index + 1.5f, bounds.getY(), width - 3.0f, bounds.getHeight() };
    }

    int stepAt (juce::Point<float> position) const
    {
        const int index = (int) (position.x / ((float) getWidth() / (float) StepSequencer::numSteps));
        return juce::isPositiveAndBelow (index, StepSequencer::numSteps) ? index : -1;
    }

    void write (int index, const StepSequencer::Step& s)
    {
        processor.setSequencerStep (index, s);
        steps[(size_t) index] = s;
        repaint();
    }

    NeonSynthProcessor& processor;
    std::array<StepSequencer::Step, StepSequencer::numSteps> steps {};
    int   playhead = -1;

    int   dragStep = -1;
    bool  dragged = false;
    float dragStartY = 0.0f;
    StepSequencer::Step dragStart;
};
