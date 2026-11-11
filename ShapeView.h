// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "NeonSynthProcessor.h"
#include "Sculpt.h"
#include "Visualisers.h"

/**
    One cycle of the Oscillator or Random wave as it leaves the Fold stage
    (v1.6.0), shown in the sample strip's place while either source plays.

    - The wave is drawn with CHOP and Fold applied; Noise is left out, since a
      single drawn cycle of noise would mislead more than it shows.
    - CHOP's 16 x Comb steps are marked along the cycle, the chopped ones shaded
      (faintly while Overtone is 0, when chopping changes nothing).
    - Click a step to toggle it; drag across steps to set them all the same way.
      Steps repeat with Comb, so a click toggles every copy of that step.
    - With Random, the drawing follows the 16 levels of the lowest sounding
      Random voice, which drift as Random re-rolls them; with none sounding it
      shows a fixed example set.

    It draws the ideal wave (no band-limiting): this is a picture of the shape,
    not of the exact samples. refresh() is called by the editor's 30 Hz timer
    and repaints only when something shown has changed. Message thread only.
*/
class ShapeView : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    ShapeView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state)
        : processor (p)
    {
        sourceValue   = state.getRawParameterValue ("source");
        waveformValue = state.getRawParameterValue ("waveform");
        overtoneValue = state.getRawParameterValue ("chopOvertone");
        combValue     = state.getRawParameterValue ("chopComb");
        driveValue    = state.getRawParameterValue ("foldDrive");
        foldMixValue  = state.getRawParameterValue ("foldMix");

        RandomStepOscillator example;   // a fixed set of levels for when no Random voice sounds
        example.reseed (1);
        exampleLevels = example.getLevels();

        setTooltip ("One cycle of the wave after CHOP and Fold. Click a step to chop it (or not); "
                    "drag across steps to set several. OVERTONE sets how far chopped steps are cut.");
    }

    /** Re-reads the settings and, for Random, the sounding levels. Message thread. */
    void refresh()
    {
        Shown now;
        now.random   = juce::roundToInt (sourceValue->load()) == (int) NeonSynthVoice::Source::random;
        now.waveform = juce::roundToInt (waveformValue->load());
        now.overtone = overtoneValue->load() * 0.01f;
        now.comb     = juce::jlimit (1, 32, juce::roundToInt (combValue->load()));
        now.drive    = driveValue->load();
        now.foldMix  = foldMixValue->load() * 0.01f;
        now.pattern  = processor.getChopPattern();
        now.levels   = exampleLevels;

        if (now.random)
        {
            float lowestHz = 0.0f;
            for (const auto& v : processor.getVoiceViews())
            {
                const float hz = v.frequencyHz.load (std::memory_order_relaxed);
                if (v.active.load (std::memory_order_relaxed) && v.playingRandom.load (std::memory_order_relaxed)
                    && hz > 0.0f && (lowestHz == 0.0f || hz < lowestHz))
                {
                    lowestHz = hz;
                    for (size_t k = 0; k < now.levels.size(); ++k)
                        now.levels[k] = v.randomLevels[k].load (std::memory_order_relaxed);
                }
            }
        }

        if (! (now == shown))
        {
            shown = now;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        neon::drawPanel (g, bounds, neon::cyan);
        const auto inner = bounds.reduced (4.0f, 3.0f);
        const int steps = Chop::stepsPerPattern * shown.comb;
        const float stepWidth = inner.getWidth() / (float) steps;

        // Chopped steps, shaded; step edges as faint lines (only where they are far enough apart to read).
        for (int s = 0; s < steps; ++s)
            if (((shown.pattern >> (s % Chop::stepsPerPattern)) & 1u) != 0)
            {
                g.setColour (neon::amber.withAlpha (shown.overtone > 0.0f ? 0.22f : 0.08f));
                g.fillRect (juce::Rectangle<float> (inner.getX() + (float) s * stepWidth, inner.getY(), stepWidth, inner.getHeight()));
            }
        if (stepWidth >= 3.0f)
        {
            g.setColour (neon::cyan.withAlpha (0.12f));
            for (int s = 1; s < steps; ++s)
                g.drawVerticalLine ((int) (inner.getX() + (float) s * stepWidth), inner.getY(), inner.getBottom());
        }

        // The centre line and the wave.
        const float mid = inner.getCentreY(), halfHeight = inner.getHeight() * 0.5f / 1.2f;   // room for a little overshoot
        g.setColour (neon::cyan.withAlpha (0.2f));
        g.drawHorizontalLine ((int) mid, inner.getX(), inner.getRight());

        juce::Path wave;
        const int points = juce::jmax (2, (int) inner.getWidth());
        for (int i = 0; i < points; ++i)
        {
            const float phase = ((float) i + 0.5f) / (float) points;
            const float y = mid - valueAt (phase) * halfHeight;
            const float x = inner.getX() + inner.getWidth() * phase;
            if (i == 0) wave.startNewSubPath (x, y);
            else        wave.lineTo (x, y);
        }
        g.setColour (neon::cyan);
        g.strokePath (wave, juce::PathStrokeType (1.5f));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int step = stepAt ((float) e.x);
        if (step < 0)
            return;
        paintChopped = ((processor.getChopPattern() >> step) & 1u) == 0;   // the click flips this step; a drag copies that
        lastStep = step;
        processor.setChopStep (step, paintChopped);
        refresh();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int step = stepAt ((float) e.x);
        if (step < 0 || step == lastStep)
            return;
        lastStep = step;
        processor.setChopStep (step, paintChopped);
        refresh();
    }

private:
    struct Shown
    {
        bool random = false;
        int waveform = 0, comb = 1;
        float overtone = 0.0f, drive = 3.0f, foldMix = 0.0f;
        juce::uint16 pattern = 0;
        std::array<float, RandomStepOscillator::numSteps> levels {};

        bool operator== (const Shown& o) const noexcept
        {
            return random == o.random && waveform == o.waveform && comb == o.comb && overtone == o.overtone
                && drive == o.drive && foldMix == o.foldMix && pattern == o.pattern && levels == o.levels;
        }
    };

    /** The ideal wave at a phase, with CHOP's gain and Fold applied (no band-limiting, no noise). */
    float valueAt (float phase) const noexcept
    {
        float x;
        if (shown.random)
            x = shown.levels[(size_t) juce::jmin ((int) (phase * (float) RandomStepOscillator::numSteps), RandomStepOscillator::numSteps - 1)];
        else switch (shown.waveform)
        {
            case PolyBlepOscillator::square:   x = phase < 0.5f ? 1.0f : -1.0f; break;
            case PolyBlepOscillator::triangle: x = phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase; break;
            case PolyBlepOscillator::sawtooth: x = 2.0f * phase - 1.0f; break;
            default:                           x = std::sin (juce::MathConstants<float>::twoPi * phase); break;
        }

        const int segment = juce::jmin ((int) (phase * (float) (Chop::stepsPerPattern * shown.comb)), Chop::stepsPerPattern * shown.comb - 1);
        if (((shown.pattern >> (segment % Chop::stepsPerPattern)) & 1u) != 0)
            x *= 1.0f - shown.overtone;

        if (shown.foldMix > 0.0f)
            x += shown.foldMix * (Sculpt::fastSin (juce::MathConstants<float>::halfPi * shown.drive * x) - x);
        return x;
    }

    /** The pattern step (0..15) under x, or -1 outside the drawing. */
    int stepAt (float x) const noexcept
    {
        const auto inner = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
        const float fraction = (x - inner.getX()) / juce::jmax (1.0f, inner.getWidth());
        if (fraction < 0.0f || fraction >= 1.0f)
            return -1;
        const int steps = Chop::stepsPerPattern * shown.comb;
        return juce::jmin ((int) (fraction * (float) steps), steps - 1) % Chop::stepsPerPattern;
    }

    NeonSynthProcessor& processor;
    std::atomic<float>* sourceValue = nullptr;
    std::atomic<float>* waveformValue = nullptr;
    std::atomic<float>* overtoneValue = nullptr;
    std::atomic<float>* combValue = nullptr;
    std::atomic<float>* driveValue = nullptr;
    std::atomic<float>* foldMixValue = nullptr;

    std::array<float, RandomStepOscillator::numSteps> exampleLevels {};
    Shown shown;
    bool paintChopped = true;
    int  lastStep = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShapeView)
};
