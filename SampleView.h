// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

#pragma once
#include <JuceHeader.h>
#include "NeonSynthProcessor.h"
#include "Visualisers.h"

/**
    The sample's waveform in the editor, with its markers and live playheads.

    - Start (cyan line), and the loop (amber region between two brackets,
      shown unless the loop mode is One-shot).
    - Drag a marker to move it; click anywhere else to set Start there.
      Markers are parameters, so the drag is one undoable, automatable gesture.
    - A thin white line per voice playing the sample shows where it reads.

    refresh() is called by the editor's 30 Hz timer; it repaints only when
    something shown has changed. Message thread only.
*/
class SampleView : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    SampleView (NeonSynthProcessor& p, juce::AudioProcessorValueTreeState& state)
        : processor (p), apvts (state)
    {
        startValue     = apvts.getRawParameterValue ("smpStart");
        loopStartValue = apvts.getRawParameterValue ("smpLoopStart");
        loopEndValue   = apvts.getRawParameterValue ("smpLoopEnd");
        loopModeValue  = apvts.getRawParameterValue ("smpLoop");
        sourceValue    = apvts.getRawParameterValue ("source");
        setTooltip ("Drag the cyan line (start) or the amber brackets (loop). Click to set the start. Drop a WAV, AIFF or FLAC file anywhere on the synth to load it.");
    }

    /** Re-reads the sample, markers and playheads. Message thread. */
    void refresh()
    {
        bool changed = false;

        // A new sample: rebuild the min/max overview (from the 1/4-rate copy: plenty for a few hundred pixels).
        processor.withCurrentSample ([&] (const SampleData* sample)
        {
            const auto hash = sample != nullptr ? sample->getHash() : juce::String();
            if (hash == shownHash && sample != nullptr && (bool) sample->getOrigin().isDefault == shownDefault)
                return;

            shownHash = hash;
            shownDefault = sample != nullptr && sample->getOrigin().isDefault;
            peaks.assign (numBuckets, { 0.0f, 0.0f });

            if (sample != nullptr)
            {
                const int level = SampleData::numLevels - 1;
                const int n = sample->getLength (level);
                const float* data = sample->getData (level);
                for (int b = 0; b < numBuckets; ++b)
                {
                    const int from = (int) ((juce::int64) b * n / numBuckets);
                    const int to   = juce::jmax (from + 1, (int) ((juce::int64) (b + 1) * n / numBuckets));
                    float lo = 0.0f, hi = 0.0f;
                    for (int i = from; i < juce::jmin (to, n); ++i)
                    {
                        lo = juce::jmin (lo, data[i]);
                        hi = juce::jmax (hi, data[i]);
                    }
                    peaks[(size_t) b] = { lo, hi };
                }
            }
            changed = true;
        });

        const Markers m { startValue->load(), loopStartValue->load(), loopEndValue->load(),
                          (int) loopModeValue->load(), (int) sourceValue->load() };
        if (! (m == markers)) { markers = m; changed = true; }

        std::array<float, NeonSynthProcessor::numVoices> heads {};
        const auto& views = processor.getVoiceViews();
        for (size_t i = 0; i < heads.size(); ++i)
            heads[i] = views[i].samplePosition.load (std::memory_order_relaxed);
        if (heads != playheads) { playheads = heads; changed = true; }

        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        neon::drawPanel (g, bounds, neon::cyan);

        const bool active = markers.source != 0;   // dimmed while the oscillator plays
        const float alpha = active ? 1.0f : 0.35f;
        const auto inner = bounds.reduced (4.0f, 3.0f);

        // Loop region
        if (markers.loopMode != SampleReader::oneShot)
        {
            const float x0 = xFor (markers.loopStart), x1 = xFor (juce::jmax (markers.loopStart, markers.loopEnd));
            g.setColour (neon::amber.withAlpha (0.10f * alpha));
            g.fillRect (juce::Rectangle<float> (x0, inner.getY(), x1 - x0, inner.getHeight()));
            g.setColour (neon::amber.withAlpha (0.85f * alpha));
            drawBracket (g, x0, inner, true);
            drawBracket (g, x1, inner, false);
        }

        // Waveform: one vertical min/max line per bucket.
        const float mid = inner.getCentreY(), halfHeight = inner.getHeight() * 0.5f;
        g.setColour (neon::cyan.withAlpha (0.75f * alpha));
        for (int b = 0; b < numBuckets; ++b)
        {
            const float x = inner.getX() + inner.getWidth() * ((float) b + 0.5f) / (float) numBuckets;
            const auto [lo, hi] = peaks[(size_t) b];
            g.drawVerticalLine ((int) x, mid - hi * halfHeight, mid - lo * halfHeight + 1.0f);
        }

        // Start marker
        g.setColour (neon::cyan.withAlpha (alpha));
        g.fillRect (juce::Rectangle<float> (xFor (markers.start) - 1.0f, inner.getY(), 2.0f, inner.getHeight()));

        // Playheads
        g.setColour (juce::Colours::white.withAlpha (0.8f * alpha));
        for (const float head : playheads)
            if (head >= 0.0f)
                g.drawVerticalLine ((int) xFor (head), inner.getY(), inner.getBottom());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const float x = (float) e.x;
        const float grab = 8.0f;
        const bool loopShown = markers.loopMode != SampleReader::oneShot;

        dragging = "smpStart";
        if (loopShown && std::abs (x - xFor (markers.loopStart)) < grab)
            dragging = "smpLoopStart";
        else if (loopShown && std::abs (x - xFor (markers.loopEnd)) < grab)
            dragging = "smpLoopEnd";

        if (auto* param = apvts.getParameter (dragging))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (fractionAt (x));
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (auto* param = apvts.getParameter (dragging))
            param->setValueNotifyingHost (fractionAt ((float) e.x));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (auto* param = apvts.getParameter (dragging))
            param->endChangeGesture();
        dragging = {};
    }

private:
    static constexpr int numBuckets = 512;

    struct Markers
    {
        float start = 0.0f, loopStart = 0.0f, loopEnd = 1.0f;
        int loopMode = 1, source = 0;
        bool operator== (const Markers& o) const noexcept
        {
            return start == o.start && loopStart == o.loopStart && loopEnd == o.loopEnd
                && loopMode == o.loopMode && source == o.source;
        }
    };

    float xFor (float fraction) const noexcept
    {
        const auto inner = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
        return inner.getX() + inner.getWidth() * juce::jlimit (0.0f, 1.0f, fraction);
    }

    float fractionAt (float x) const noexcept
    {
        const auto inner = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
        return juce::jlimit (0.0f, 1.0f, (x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
    }

    static void drawBracket (juce::Graphics& g, float x, juce::Rectangle<float> r, bool opening)
    {
        const float tick = opening ? 5.0f : -5.0f;
        g.fillRect (juce::Rectangle<float> (x - 1.0f, r.getY(), 2.0f, r.getHeight()));
        g.fillRect (juce::Rectangle<float> (juce::jmin (x, x + tick), r.getY(), std::abs (tick), 2.0f));
        g.fillRect (juce::Rectangle<float> (juce::jmin (x, x + tick), r.getBottom() - 2.0f, std::abs (tick), 2.0f));
    }

    NeonSynthProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    std::atomic<float>* startValue = nullptr;
    std::atomic<float>* loopStartValue = nullptr;
    std::atomic<float>* loopEndValue = nullptr;
    std::atomic<float>* loopModeValue = nullptr;
    std::atomic<float>* sourceValue = nullptr;

    juce::String shownHash;
    bool shownDefault = false;
    std::vector<std::pair<float, float>> peaks = std::vector<std::pair<float, float>> ((size_t) numBuckets);
    Markers markers;
    std::array<float, NeonSynthProcessor::numVoices> playheads {};
    juce::String dragging;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleView)
};
