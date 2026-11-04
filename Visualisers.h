#pragma once
#include <JuceHeader.h>
#include "NeonSynthProcessor.h"

/*
    GUI-side visualisers for Neon Synth.

    Everything here runs on the message thread. Audio reaches it only through
    the processor's lock-free scope FIFO and the per-voice atomics, both read
    from the editor's timer - nothing in this file touches the audio thread.
*/

namespace neon
{
    inline const juce::Colour cyan    { 0xff00ffff };
    inline const juce::Colour magenta { 0xffff00ff };
    inline const juce::Colour amber   { 0xffffcc00 };
    inline const juce::Colour red     { 0xffff3355 };
    inline const juce::Colour panel   { 0xd8050a12 };

    inline juce::Font font (float height, bool bold = false)
    {
        auto options = juce::FontOptions (height);
        return juce::Font (bold ? options.withStyle ("Bold") : options);
    }

    inline void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour edge)
    {
        g.setColour (panel);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (edge.withAlpha (0.35f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    }

    /** Strokes a path with a soft neon glow: two wide faint passes under a thin bright one. */
    inline void strokeGlow (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width = 1.4f)
    {
        const auto stroke = [&] (float w, float alpha)
        {
            g.setColour (c.withMultipliedAlpha (alpha));
            g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        stroke (width * 5.0f, 0.10f);
        stroke (width * 2.5f, 0.28f);
        stroke (width, 1.0f);
    }

    inline juce::String formatTime (float seconds)
    {
        return seconds < 1.0f ? juce::String (juce::roundToInt (seconds * 1000.0f)) + " ms"
                              : juce::String (seconds, 2) + " s";
    }

    inline juce::String formatHz (float hz)
    {
        return hz < 1000.0f ? juce::String (juce::roundToInt (hz)) + " Hz"
                            : juce::String (hz / 1000.0f, hz < 10000.0f ? 2 : 1) + " kHz";
    }
}

//==============================================================================
/** Rolling copy of the most recent output samples, filled from the scope FIFO. */
class SignalHistory
{
public:
    static constexpr int size = 8192;   // power of two

    void push (const float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            data[(size_t) writePos] = samples[i];
            writePos = (writePos + 1) & (size - 1);
        }
    }

    /** Copies the newest n samples (n <= size) into dest, oldest first. */
    void copyLatest (float* dest, int n) const noexcept
    {
        const int start = (writePos - n) & (size - 1);
        for (int i = 0; i < n; ++i)
            dest[i] = data[(size_t) ((start + i) & (size - 1))];
    }

private:
    std::array<float, size> data {};
    int writePos = 0;
};

//==============================================================================
/**
    Triggered oscilloscope.

    Starts each frame on a rising zero crossing so the trace stands still. When
    notes are held it locks to the lowest note's period (the synth knows it
    exactly) and shows two cycles; otherwise it free-runs over ~20 ms.
*/
class OscilloscopeView : public juce::Component
{
public:
    void update (const SignalHistory& history, double sampleRate, float lowestNoteHz)
    {
        history.copyLatest (recent.data(), searchLength);

        const double period = (lowestNoteHz > 0.0f && sampleRate > 0.0) ? sampleRate / lowestNoteHz : 0.0;
        displayLength = period > 0.0 ? juce::jlimit (128, maxDisplay, (int) std::lround (2.0 * period)) : 1024;

        // Walk back from the newest possible start to the nearest rising zero crossing.
        const int latestStart = searchLength - displayLength;
        const int span        = period > 0.0 ? (int) std::ceil (period) + 2 : 1024;
        int start = latestStart;

        for (int i = latestStart; i > juce::jmax (1, latestStart - span); --i)
        {
            if (recent[(size_t) i - 1] < 0.0f && recent[(size_t) i] >= 0.0f)
            {
                start = i;
                break;
            }
        }

        float peak = 0.0f;
        for (int i = 0; i < displayLength; ++i)
        {
            display[(size_t) i] = recent[(size_t) (start + i)];
            peak = juce::jmax (peak, std::abs (display[(size_t) i]));
        }

        // Auto-range with a slow release so the trace keeps a steady size.
        range  = juce::jmax (peak, range * 0.92f, 0.02f);
        lockHz = lowestNoteHz;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        neon::drawPanel (g, bounds, neon::cyan);
        auto area = bounds.reduced (8.0f);

        g.setColour (neon::cyan.withAlpha (0.08f));
        for (int i = 1; i < 8; ++i)
        {
            const float x = area.getX() + area.getWidth() * (float) i / 8.0f;
            g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
        }
        for (int i = 1; i < 4; ++i)
        {
            const float y = area.getY() + area.getHeight() * (float) i / 4.0f;
            g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
        }

        juce::Path trace;
        const float midY   = area.getCentreY();
        const float yScale = area.getHeight() * 0.45f / range;

        for (int i = 0; i < displayLength; ++i)
        {
            const float x = area.getX() + area.getWidth() * (float) i / (float) (displayLength - 1);
            const float y = midY - display[(size_t) i] * yScale;
            if (i == 0) trace.startNewSubPath (x, y);
            else        trace.lineTo (x, y);
        }
        neon::strokeGlow (g, trace, neon::cyan);

        g.setFont (neon::font (10.0f));
        g.setColour (neon::cyan.withAlpha (0.7f));
        juce::String label ("free run");
        if (lockHz > 0.0f)
        {
            const int note = juce::roundToInt (12.0f * std::log2 (lockHz / 440.0f)) + 69;
            label = juce::MidiMessage::getMidiNoteName (note, true, true, 4) + "  " + neon::formatHz (lockHz);
        }
        g.drawText (label, area.removeFromTop (14.0f), juce::Justification::topRight);
    }

private:
    static constexpr int searchLength = 4096;
    static constexpr int maxDisplay   = 2048;

    std::array<float, searchLength> recent {};
    std::array<float, maxDisplay>   display {};
    int   displayLength = 1024;
    float range = 0.25f;
    float lockHz = 0.0f;
};

//==============================================================================
/**
    Spectrum analyser (4096-point FFT, log frequency axis) with the low-pass
    filter's response drawn on top. The filter curve is computed from the
    cutoff parameter, so it moves the instant the knob does.
*/
class SpectrumView : public juce::Component
{
public:
    SpectrumView()
    {
        for (size_t n = 0; n < (size_t) fftSize; ++n)
        {
            window[n] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) n / (float) (fftSize - 1));
            windowSum += window[n];
        }
        levels.fill (minDb);
    }

    void update (const SignalHistory& history, double newSampleRate, float newCutoffHz, bool newFilterOn)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        cutoffHz   = newCutoffHz;
        filterOn   = newFilterOn;

        history.copyLatest (fftData.data(), fftSize);
        for (size_t n = 0; n < (size_t) fftSize; ++n)
            fftData[n] *= window[n];
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const double binHz = sampleRate / fftSize;
        const float  norm  = 2.0f / windowSum;   // full-scale sine -> 0 dBFS
        constexpr float fallPerFrame = 1.5f;     // dB, gives peaks a gentle decay

        for (int i = 0; i < numPoints; ++i)
        {
            const double lo = frequencyAt ((double) i / numPoints) / binHz;
            const double hi = frequencyAt ((double) (i + 1) / numPoints) / binHz;
            float magnitude = 0.0f;

            if (hi - lo < 1.0)   // narrower than a bin: interpolate at the band centre
            {
                const double centre = 0.5 * (lo + hi);
                const int    k      = juce::jlimit (0, fftSize / 2 - 2, (int) centre);
                const float  frac   = (float) (centre - k);
                magnitude = fftData[(size_t) k] + frac * (fftData[(size_t) k + 1] - fftData[(size_t) k]);
            }
            else                 // wider: take the loudest bin in the band
            {
                const int first = juce::jlimit (0, fftSize / 2 - 1, (int) std::ceil (lo));
                const int last  = juce::jlimit (0, fftSize / 2 - 1, (int) std::floor (hi));
                for (int k = first; k <= last; ++k)
                    magnitude = juce::jmax (magnitude, fftData[(size_t) k]);
            }

            const float db = juce::Decibels::gainToDecibels (magnitude * norm, minDb);
            levels[(size_t) i] = juce::jmax (db, levels[(size_t) i] - fallPerFrame);
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        neon::drawPanel (g, bounds, neon::cyan);
        auto area = bounds.reduced (8.0f);
        area.removeFromBottom (12.0f);   // room for frequency labels

        // Grid and labels
        g.setFont (neon::font (9.0f));
        for (const float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            if (f >= maxHz()) continue;
            const float x = xFor (area, f);
            g.setColour (neon::cyan.withAlpha (0.08f));
            g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
            g.setColour (neon::cyan.withAlpha (0.5f));
            g.drawText (f < 1000.0f ? juce::String ((int) f) : juce::String ((int) (f / 1000.0f)) + "k",
                        juce::Rectangle<float> (x - 15.0f, area.getBottom() + 1.0f, 30.0f, 11.0f),
                        juce::Justification::centred);
        }
        for (const float db : { -24.0f, -48.0f, -72.0f })
        {
            const float y = yFor (area, db);
            g.setColour (neon::cyan.withAlpha (0.08f));
            g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
            g.setColour (neon::cyan.withAlpha (0.4f));
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (area.getX() + 2.0f, y - 11.0f, 30.0f, 10.0f),
                        juce::Justification::left);
        }

        // Spectrum: filled area plus a glowing outline
        juce::Path outline;
        for (int i = 0; i < numPoints; ++i)
        {
            const float x = area.getX() + area.getWidth() * ((float) i + 0.5f) / (float) numPoints;
            const float y = yFor (area, levels[(size_t) i]);
            if (i == 0) outline.startNewSubPath (x, y);
            else        outline.lineTo (x, y);
        }
        auto fill = outline;
        fill.lineTo (area.getRight(), area.getBottom());
        fill.lineTo (area.getX(), area.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (neon::cyan.withAlpha (0.30f), 0.0f, area.getY(),
                                                 neon::cyan.withAlpha (0.02f), 0.0f, area.getBottom(), false));
        g.fillPath (fill);
        neon::strokeGlow (g, outline, neon::cyan, 1.1f);

        // Filter response (2-pole Butterworth low-pass via the bilinear transform,
        // which is what StateVariableTPTFilter with Q = 1/sqrt(2) implements)
        const float fc = juce::jlimit (20.0f, 0.45f * (float) sampleRate, cutoffHz);
        const auto  colour = filterOn ? neon::magenta : neon::magenta.withAlpha (0.3f);
        juce::Path response;
        for (int i = 0; i <= numPoints; ++i)
        {
            const float f = (float) frequencyAt ((double) i / numPoints);
            float db = 0.0f;
            if (filterOn)
            {
                const double ratio = std::tan (juce::MathConstants<double>::pi * f / sampleRate)
                                   / std::tan (juce::MathConstants<double>::pi * fc / sampleRate);
                db = juce::Decibels::gainToDecibels ((float) (1.0 / std::sqrt (1.0 + std::pow (ratio, 4.0))), minDb);
            }
            const float x = area.getX() + area.getWidth() * (float) i / (float) numPoints;
            if (i == 0) response.startNewSubPath (x, yFor (area, db));
            else        response.lineTo (x, yFor (area, db));
        }
        neon::strokeGlow (g, response, colour, 1.2f);

        if (filterOn)
        {
            const float x = xFor (area, fc);
            g.setColour (neon::magenta.withAlpha (0.35f));
            g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
            g.setColour (neon::magenta);
            g.setFont (neon::font (10.0f));
            const bool labelLeft = x > area.getRight() - 70.0f;
            g.drawText (neon::formatHz (fc),
                        juce::Rectangle<float> (labelLeft ? x - 64.0f : x + 4.0f, area.getY(), 60.0f, 12.0f),
                        labelLeft ? juce::Justification::right : juce::Justification::left);
        }
    }

private:
    static constexpr int   fftOrder  = 12;
    static constexpr int   fftSize   = 1 << fftOrder;
    static constexpr int   numPoints = 160;
    static constexpr float minDb     = -96.0f;
    static constexpr float minHz     = 20.0f;

    double maxHz() const noexcept { return juce::jmin (20000.0, 0.5 * sampleRate); }

    double frequencyAt (double proportion) const noexcept
    {
        return minHz * std::pow (maxHz() / minHz, proportion);
    }

    float xFor (juce::Rectangle<float> area, float hz) const noexcept
    {
        const double p = std::log (hz / minHz) / std::log (maxHz() / minHz);
        return area.getX() + area.getWidth() * (float) p;
    }

    static float yFor (juce::Rectangle<float> area, float db) noexcept
    {
        return juce::jmap (juce::jlimit (minDb, 0.0f, db), minDb, 0.0f, area.getBottom(), area.getY());
    }

    juce::dsp::FFT fft { fftOrder };
    std::array<float, 2 * fftSize> fftData {};
    std::array<float, fftSize> window {};
    float windowSum = 0.0f;
    std::array<float, numPoints> levels {};

    double sampleRate = 44100.0;
    float  cutoffHz = 2000.0f;
    bool   filterOn = true;
};

//==============================================================================
/**
    Stereo output meter: an RMS bar per channel, a peak line, a 1.5 s peak hold
    and a shared clip light. The output never exceeds the soft clipper's -1 dBFS
    ceiling, so the light shows when the clipper is working hard (peaks above
    -2 dBFS) rather than digital overs. Click to reset the hold and the light.
*/
class LevelMeter : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    LevelMeter() { setTooltip ("Output level L / R (dBFS). Red light: the output soft clipper is near its -1 dBFS ceiling. "
                               "Click to reset the peak hold and the light."); }

    void update (const float* left, const float* right, int numSamples, int framesPerSecond)
    {
        const float fall = 24.0f / (float) framesPerSecond;   // 24 dB per second
        bool clipped = false;

        channels[0].update (left,  numSamples, fall, framesPerSecond * 3 / 2, clipped);
        channels[1].update (right, numSamples, fall, framesPerSecond * 3 / 2, clipped);

        if (clipped)             clipFrames = framesPerSecond * 2;
        else if (clipFrames > 0) --clipFrames;

        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        clipFrames = 0;
        for (auto& c : channels)
            c.heldPeak = minDb;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        neon::drawPanel (g, bounds, neon::cyan);
        bounds = bounds.reduced (4.0f);

        // Clip light
        const auto led = bounds.removeFromTop (10.0f);
        const bool clipping = clipFrames > 0;
        g.setColour (neon::red.withAlpha (clipping ? 0.35f : 0.12f));
        g.fillRoundedRectangle (led.expanded (clipping ? 2.0f : 0.0f), 3.0f);
        g.setColour (clipping ? neon::red : neon::red.withAlpha (0.35f));
        g.fillRoundedRectangle (led, 2.0f);
        bounds.removeFromTop (4.0f);

        // L / R captions
        auto captions = bounds.removeFromBottom (11.0f);
        g.setFont (neon::font (9.0f));
        g.setColour (neon::cyan.withAlpha (0.6f));
        g.drawText ("L", captions.removeFromLeft (captions.getWidth() * 0.5f), juce::Justification::centred);
        g.drawText ("R", captions, juce::Justification::centred);

        const auto yFor = [&] (float db)
        {
            return juce::jmap (juce::jlimit (minDb, maxDb, db), minDb, maxDb, bounds.getBottom(), bounds.getY());
        };

        juce::ColourGradient gradient (neon::cyan, 0.0f, bounds.getBottom(), neon::red, 0.0f, bounds.getY(), false);
        gradient.addColour ((double) ((-12.0f - minDb) / (maxDb - minDb)), neon::cyan);
        gradient.addColour ((double) ((-3.0f - minDb) / (maxDb - minDb)), neon::amber);

        const float barWidth = bounds.getWidth() * 0.32f;
        for (size_t ch = 0; ch < channels.size(); ++ch)
        {
            const auto& c = channels[ch];
            const float centre = bounds.getX() + bounds.getWidth() * (ch == 0 ? 0.27f : 0.73f);
            const auto  bar = juce::Rectangle<float> (centre - barWidth * 0.5f, bounds.getY(), barWidth, bounds.getHeight());

            g.setColour (neon::cyan.withAlpha (0.08f));
            g.fillRect (bar);
            g.setGradientFill (gradient);
            g.fillRect (bar.withTop (yFor (c.rmsLevel)));

            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.fillRect (bar.getX(), yFor (c.peakLevel) - 1.0f, bar.getWidth(), 2.0f);
            g.setColour (neon::amber);
            g.fillRect (bar.getX() - 1.0f, yFor (c.heldPeak) - 0.75f, bar.getWidth() + 2.0f, 1.5f);
        }

        // 0 dBFS tick
        g.setColour (neon::red.withAlpha (0.6f));
        g.drawHorizontalLine (juce::roundToInt (yFor (0.0f)), bounds.getX(), bounds.getRight());
    }

private:
    static constexpr float minDb = -60.0f;
    static constexpr float maxDb = 6.0f;
    static constexpr float clipLightThreshold = 0.79432823f;   // -2 dBFS: the soft clipper is close to its ceiling

    struct Channel
    {
        float rmsLevel = minDb, peakLevel = minDb, heldPeak = minDb;
        int   holdFrames = 0;

        void update (const float* samples, int numSamples, float fall, int holdLength, bool& clipped)
        {
            float peak = 0.0f, sumSquares = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                peak = juce::jmax (peak, std::abs (samples[i]));
                sumSquares += samples[i] * samples[i];
            }
            clipped = clipped || peak >= clipLightThreshold;

            const float rms    = numSamples > 0 ? std::sqrt (sumSquares / (float) numSamples) : 0.0f;
            const float peakDb = juce::Decibels::gainToDecibels (peak, minDb);

            rmsLevel  = juce::jmax (juce::Decibels::gainToDecibels (rms, minDb), rmsLevel - fall);
            peakLevel = juce::jmax (peakDb, peakLevel - fall);

            if (peakDb >= heldPeak)  { heldPeak = peakDb; holdFrames = holdLength; }
            else if (holdFrames > 0) { --holdFrames; }
            else                     { heldPeak = juce::jmax (peakDb, heldPeak - fall); }
        }
    };

    std::array<Channel, 2> channels {};
    int clipFrames = 0;
};

//==============================================================================
/**
    ADSR shape drawn from the four knobs, with a dot for every sounding voice
    showing where it is in its envelope and how loud.

    Segment widths follow the square root of each time so a 10 s release does
    not squash a 10 ms attack into nothing.
*/
class AdsrView : public juce::Component
{
public:
    using VoiceViews = std::array<NeonSynthProcessor::VoiceView, NeonSynthProcessor::numVoices>;

    void update (float attack, float decay, float sustain, float release, const VoiceViews& voices)
    {
        constexpr float sustainWidth = 0.22f;
        const float a = std::sqrt (attack), d = std::sqrt (decay), r = std::sqrt (release);
        const float total = a + d + r;
        attackW  = juce::jmax (0.04f, (1.0f - sustainWidth) * a / total);
        decayW   = juce::jmax (0.04f, (1.0f - sustainWidth) * d / total);
        releaseW = juce::jmax (0.04f, (1.0f - sustainWidth) * r / total);
        const float scale = (1.0f - sustainWidth) / (attackW + decayW + releaseW);
        attackW *= scale;  decayW *= scale;  releaseW *= scale;
        sustainLevel = sustain;

        numMarkers = 0;
        for (const auto& v : voices)
        {
            if (! v.active.load (std::memory_order_relaxed))
                continue;

            const float on  = v.secondsSinceOn.load (std::memory_order_relaxed);
            const float off = v.secondsSinceOff.load (std::memory_order_relaxed);
            float x;

            if (off >= 0.0f)            x = releaseStart() + releaseW * juce::jmin (1.0f, off / release);
            else if (on < attack)       x = attackW * on / attack;
            else if (on < attack + decay) x = attackW + decayW * (on - attack) / decay;
            else                        x = sustainStart() + sustainWidth * 0.9f * juce::jmin (1.0f, (on - attack - decay) / 2.0f);

            markers[(size_t) numMarkers++] = { x, v.level.load (std::memory_order_relaxed) };
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        neon::drawPanel (g, bounds, neon::amber);
        const auto area = bounds.reduced (8.0f, 6.0f);

        const auto pointAt = [&] (float x01, float level)
        {
            return juce::Point<float> (area.getX() + area.getWidth() * x01,
                                       area.getBottom() - area.getHeight() * level);
        };

        juce::Path shape;
        shape.startNewSubPath (pointAt (0.0f, 0.0f));
        shape.lineTo (pointAt (attackW, 1.0f));
        shape.lineTo (pointAt (sustainStart(), sustainLevel));
        shape.lineTo (pointAt (releaseStart(), sustainLevel));
        shape.lineTo (pointAt (1.0f, 0.0f));

        auto fill = shape;
        fill.closeSubPath();
        g.setColour (neon::amber.withAlpha (0.10f));
        g.fillPath (fill);

        g.setColour (neon::amber.withAlpha (0.15f));
        for (const float x : { attackW, sustainStart(), releaseStart() })
            g.drawVerticalLine (juce::roundToInt (pointAt (x, 0.0f).x), area.getY(), area.getBottom());

        neon::strokeGlow (g, shape, neon::amber, 1.3f);

        for (int i = 0; i < numMarkers; ++i)
        {
            const auto p = pointAt (markers[(size_t) i].x, markers[(size_t) i].level);
            g.setColour (neon::amber.withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (p));
            g.setColour (juce::Colours::white);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (p));
        }
    }

private:
    struct Marker { float x = 0.0f, level = 0.0f; };

    float sustainStart() const noexcept { return attackW + decayW; }
    float releaseStart() const noexcept { return 1.0f - releaseW; }

    float attackW = 0.2f, decayW = 0.2f, releaseW = 0.38f, sustainLevel = 0.7f;
    std::array<Marker, NeonSynthProcessor::numVoices> markers {};
    int numMarkers = 0;
};

//==============================================================================
/** One light per voice; brightness follows that voice's envelope, with a short afterglow. */
class VoiceLeds : public juce::Component
{
public:
    void update (const AdsrView::VoiceViews& voices)
    {
        bool changed = false;
        for (size_t i = 0; i < voices.size(); ++i)
        {
            const float target = voices[i].active.load (std::memory_order_relaxed)
                               ? voices[i].level.load (std::memory_order_relaxed) : 0.0f;
            const float next = juce::jmax (target, glow[i] * 0.75f);
            if (std::abs (next - glow[i]) > 0.002f) { glow[i] = next; changed = true; }
        }
        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds  = getLocalBounds().toFloat();
        const float step   = bounds.getWidth() / (float) glow.size();
        const float radius = juce::jmin (step, bounds.getHeight()) * 0.30f;

        for (size_t i = 0; i < glow.size(); ++i)
        {
            const juce::Point<float> c (bounds.getX() + step * ((float) i + 0.5f), bounds.getCentreY());
            const float level = glow[i];

            if (level > 0.01f)
            {
                g.setColour (neon::cyan.withAlpha (0.35f * level));
                g.fillEllipse (juce::Rectangle<float> (radius * 4.4f, radius * 4.4f).withCentre (c));
                g.setColour (neon::cyan.withAlpha (0.25f + 0.75f * std::sqrt (level)));
                g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c));
            }
            else
            {
                g.setColour (neon::cyan.withAlpha (0.25f));
                g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c), 1.0f);
            }
        }
    }

private:
    std::array<float, NeonSynthProcessor::numVoices> glow {};
};
