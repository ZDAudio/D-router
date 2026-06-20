#pragma once

#include "DSP/Builtin/ResonanceSuppressorProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

namespace dcr::builtin
{

// Interactive editor for the Resonance Suppressor (peer of SpectralAutoEqEditor).
// Top: a log-frequency graph showing the live 1/N-octave spectrum (filled), the
// smooth detection baseline (faint), the applied reduction curve (cyan), and the
// editable threshold-offset curve (amber line + node handles).  Drag a node
// vertically to set its dB offset (added to the global Depth threshold: down =
// chase resonances harder there, up = protect); drag horizontally to *sweep*
// (paint) across several nodes; double-click a node to zero it.  A label reports
// the analysis window / FFT size / overlap / resolution.  Below: the generic
// parameter list (resolution, depth, sharpness, max reduction, attack, release,
// min freq) which stays two-way in sync via the APVTS.
class ResonanceSuppressorEditor : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    explicit ResonanceSuppressorEditor (ResonanceSuppressorProcessor& p)
        : juce::AudioProcessorEditor (p), proc (p)
    {
        generic = std::make_unique<juce::GenericAudioProcessorEditor> (proc);
        viewport.setViewedComponent (generic.get(), false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        setSize (560, 560);
        startTimerHz (30);
    }
    ~ResonanceSuppressorEditor() override { stopTimer(); }

    void resized() override
    {
        auto r = getLocalBounds();
        graphArea = r.removeFromTop (graphH);
        viewport.setBounds (r);
        generic->setSize (r.getWidth() - 10, juce::jmax (generic->getHeight(), 7 * 26 + 30));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour::fromRGB (14, 14, 18));
        auto a = graphArea.reduced (8);
        g.setColour (juce::Colour::fromRGB (20, 20, 26));
        g.fillRect (a);
        plot = a.toFloat();

        drawGrid (g);
        drawSpectrum (g);
        drawBaseline (g);
        drawReduction (g);
        drawThreshold (g);
        drawLabel (g);
    }

    // ----- interaction (node drag + horizontal sweep) -----------------------
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! plot.toNearestInt().contains (e.getPosition())) { lastNode = -1; return; }
        lastNode = nearestNode (e.position.x);
        applyNodeAt (lastNode, e.position.y);
        repaint (graphArea);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (lastNode < 0) return;
        const float x = juce::jlimit (plot.getX(), plot.getRight(), e.position.x);
        const int cur = nearestNode (x);
        const int lo = juce::jmin (lastNode, cur), hi = juce::jmax (lastNode, cur);
        for (int i = lo; i <= hi; ++i) applyNodeAt (i, e.position.y);   // sweep paints across nodes
        lastNode = cur;
        repaint (graphArea);
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! plot.toNearestInt().contains (e.getPosition())) return;
        proc.resetNode (nearestNode (e.position.x));
        repaint (graphArea);
    }

private:
    void timerCallback() override { repaint (graphArea); }

    static constexpr int   graphH = 240;
    static constexpr float fLo = 20.0f, fHi = 20000.0f;
    static constexpr float magLo = -90.0f, magHi = 0.0f;   // spectrum scale (dBFS)
    static constexpr float gRange = 24.0f;                  // reduction / threshold scale (dB)
    static constexpr float nodeRange = 18.0f;               // threshold-offset clamp (+/- dB)

    // ---- coordinate mapping ------------------------------------------------
    float xForFreq (float f) const
    {
        return juce::jmap (std::log10 (juce::jlimit (fLo, fHi, f)),
                           std::log10 (fLo), std::log10 (fHi), plot.getX(), plot.getRight());
    }
    float freqForX (float x) const
    {
        return std::pow (10.0f, juce::jmap (x, plot.getX(), plot.getRight(),
                                            std::log10 (fLo), std::log10 (fHi)));
    }
    float yForMag  (float db) const { return juce::jmap (juce::jlimit (magLo, magHi, db), magLo, magHi, plot.getBottom(), plot.getY()); }
    float yForGain (float db) const { return juce::jmap (juce::jlimit (-gRange, gRange, db), gRange, -gRange, plot.getY(), plot.getBottom()); }
    float gainForY (float y)  const { return juce::jmap (y, plot.getY(), plot.getBottom(), gRange, -gRange); }

    int nearestNode (float x) const
    {
        int best = 0; float bd = 1.0e9f;
        for (int i = 0; i < ResonanceSuppressorProcessor::kNumNodes; ++i)
        {
            const float d = std::abs (xForFreq (ResonanceSuppressorProcessor::nodeFreq (i)) - x);
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
    void applyNodeAt (int i, float y)
    {
        const float db = gainForY (juce::jlimit (plot.getY(), plot.getBottom(), y));
        proc.setNodeDb (i, juce::jlimit (-nodeRange, nodeRange, db));
    }

    float curveDbAtFreq (float f) const
    {
        const auto ni = resonance::nodeInterp (f, ResonanceSuppressorProcessor::kNumNodes);
        return proc.getNodeDb (ni.lo) + (proc.getNodeDb (ni.lo + 1) - proc.getNodeDb (ni.lo)) * ni.frac;
    }

    // ---- drawing -----------------------------------------------------------
    void drawGrid (juce::Graphics& g)
    {
        g.setColour (juce::Colour::fromRGB (40, 40, 48));
        for (float db = -gRange; db <= gRange + 0.1f; db += 6.0f)
            g.drawHorizontalLine ((int) yForGain (db), plot.getX(), plot.getRight());
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const float x = xForFreq (f);
            g.setColour (juce::Colour::fromRGB (40, 40, 48));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (juce::Colour::fromRGB (90, 90, 100));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k" : juce::String ((int) f),
                        (int) x + 2, (int) plot.getBottom() - 13, 36, 12, juce::Justification::topLeft);
        }
        g.setColour (juce::Colour::fromRGB (70, 70, 82));      // 0 dB line
        g.drawHorizontalLine ((int) yForGain (0.0f), plot.getX(), plot.getRight());
    }

    void drawSpectrum (juce::Graphics& g)
    {
        const int n = proc.getDisplaySize();
        if (n < 2) return;
        juce::Path fill, line;
        fill.startNewSubPath (plot.getX(), plot.getBottom());
        for (int i = 0; i < n; ++i)
        {
            const float x = xForFreq (proc.getDisplayFreq (i));
            const float y = yForMag (proc.getDisplayMagDb (i));
            fill.lineTo (x, y);
            if (i == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
        }
        fill.lineTo (plot.getRight(), plot.getBottom());
        fill.closeSubPath();
        g.setColour (juce::Colour::fromRGB (60, 120, 150).withAlpha (0.35f));
        g.fillPath (fill);
        g.setColour (juce::Colour::fromRGB (90, 170, 200));
        g.strokePath (line, juce::PathStrokeType (1.0f));
    }

    void drawBaseline (juce::Graphics& g)
    {
        const int n = proc.getDisplaySize();
        if (n < 2) return;
        juce::Path p;
        for (int i = 0; i < n; ++i)
        {
            const float x = xForFreq (proc.getDisplayFreq (i));
            const float y = yForMag (proc.getDisplayBaseDb (i));
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (juce::Colour::fromRGB (130, 130, 140).withAlpha (0.6f));
        const float dashes[] = { 4.0f, 3.0f };
        juce::Path dashed;
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, p, dashes, 2);
        g.fillPath (dashed);
    }

    void drawReduction (juce::Graphics& g)
    {
        const int n = proc.getDisplaySize();
        if (n < 2) return;
        juce::Path p;
        for (int i = 0; i < n; ++i)
        {
            const float x = xForFreq (proc.getDisplayFreq (i));
            const float y = yForGain (proc.getDisplayGainDb (i));
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (juce::Colour::fromRGB (0, 255, 210));
        g.strokePath (p, juce::PathStrokeType (2.0f));
    }

    void drawThreshold (juce::Graphics& g)
    {
        juce::Path p;
        const int x0 = (int) plot.getX(), x1 = (int) plot.getRight();
        for (int x = x0; x <= x1; ++x)
        {
            const float y = yForGain (curveDbAtFreq (freqForX ((float) x)));
            if (x == x0) p.startNewSubPath ((float) x, y); else p.lineTo ((float) x, y);
        }
        g.setColour (juce::Colour::fromRGB (255, 180, 60).withAlpha (0.9f));
        g.strokePath (p, juce::PathStrokeType (1.5f));

        for (int i = 0; i < ResonanceSuppressorProcessor::kNumNodes; ++i)
        {
            const float x = xForFreq (ResonanceSuppressorProcessor::nodeFreq (i));
            const float y = yForGain (proc.getNodeDb (i));
            g.setColour (juce::Colour::fromRGB (255, 180, 60));
            g.fillEllipse (x - 3.0f, y - 3.0f, 6.0f, 6.0f);
        }
    }

    void drawLabel (juce::Graphics& g)
    {
        const juce::String txt = juce::String (proc.labelWindow()) + " \xc2\xb7 "
            + juce::String (proc.labelFftSize()) + " \xc2\xb7 "
            + juce::String (proc.labelOverlapPct()) + "% \xc2\xb7 1/" + juce::String (proc.resolutionN()) + " oct";
        g.setColour (juce::Colour::fromRGB (150, 150, 160));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (txt, (int) plot.getX() + 4, (int) plot.getY() + 3, 380, 14, juce::Justification::topLeft);
    }

    ResonanceSuppressorProcessor& proc;
    std::unique_ptr<juce::GenericAudioProcessorEditor> generic;
    juce::Viewport viewport;
    juce::Rectangle<int>   graphArea;
    juce::Rectangle<float> plot;
    int lastNode = -1;
};

} // namespace dcr::builtin
