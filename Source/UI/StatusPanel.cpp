#include "UI/StatusPanel.h"

#include "Engine/AudioEngine.h"

namespace dcr {

StatusPanel::StatusPanel (AudioEngine& e) : engine (e)
{
    title.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId,
                     juce::Colour (0xFF000000u | engine.getSettings().accentColorRGB));
    addAndMakeVisible (title);

    popOutBtn.setName ("win");
    popOutBtn.setTooltip ("Pop status panel out into its own window");
    popOutBtn.onClick = [this] { if (onPopOutRequested) onPopOutRequested(); };
    addAndMakeVisible (popOutBtn);

    addAndMakeVisible (gauges);

    body.setMultiLine (true);
    body.setReadOnly (true);
    body.setScrollbarsShown (true);
    body.setCaretVisible (false);
    body.setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (12, 12, 14));
    body.setColour (juce::TextEditor::textColourId,
                    juce::Colour (0xFF000000u | engine.getSettings().accentColorRGB));
    body.setFont   (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, 0));
    addAndMakeVisible (body);

    startTimer (engine.getSettings().statusTimerMs);
    refresh();
}

void StatusPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (10, 10, 12));
    g.setColour (juce::Colour::fromRGB (36, 36, 42));
    g.drawRect (getLocalBounds(), 1);
}

void StatusPanel::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto top = r.removeFromTop (20);
    popOutBtn.setBounds (top.removeFromRight (32));
    title.setBounds (top);
    r.removeFromTop (4);

    // Top gauges occupy a fixed 64 px strip; latency text fills the rest.
    gauges.setBounds (r.removeFromTop (64));
    r.removeFromTop (6);
    body.setBounds (r);
}

void StatusPanel::visibilityChanged()
{
    if (isVisible())
        startTimer (juce::jmax (50, engine.getSettings().statusTimerMs));
    else
        stopTimer();
}

void StatusPanel::resumeUpdates()
{
    if (isVisible())
        startTimer (juce::jmax (50, engine.getSettings().statusTimerMs));
}

void StatusPanel::refresh()
{
    juce::String s;

    s << "Engine " << (int) engine.getEngineSampleRate() << " Hz / "
      << engine.getEngineBlockSize() << " spl   ";

    if (engine.getDeviceInfo().empty())
    {
        s << "(no devices selected)\n";
        body.setText (s, juce::dontSendNotification);
        body.applyColourToAllText (juce::Colour::fromRGB (160, 160, 160), true);
        return;
    }

    const int nIn  = engine.getRoutingMatrix().getNumInputs();
    const int nOut = engine.getRoutingMatrix().getNumOutputs();
    s << nIn << " in / " << nOut << " out\n";

    // --- performance line ----------------------------------------------------
    const auto processedBlocks = engine.getMatrixBlocksProcessed();
    const auto stalledBlocks   = engine.getMatrixBlocksStalled();

    // Windowed (per-refresh) stalled ratio — much more meaningful than the
    // lifetime average, which is pinned high by start-up stalls.
    if (firstSample)
    {
        firstSample = false;
        prevProcessedBlocks = processedBlocks;
        prevStalledBlocks   = stalledBlocks;
    }
    else
    {
        const uint64_t dP = processedBlocks > prevProcessedBlocks
                              ? processedBlocks - prevProcessedBlocks : 0;
        const uint64_t dS = stalledBlocks  > prevStalledBlocks
                              ? stalledBlocks  - prevStalledBlocks  : 0;
        const uint64_t dT = dP + dS;
        if (dT > 0)
            windowStalledRatio = (float) dS / (float) dT;
        else
            windowStalledRatio = 0.0f;
        prevProcessedBlocks = processedBlocks;
        prevStalledBlocks   = stalledBlocks;
    }
    const float stalledRatio = windowStalledRatio;

    const float cpuAvg  = engine.getCpuLoadAvg();
    const float cpuPeak = engine.getCpuLoadPeak();

    s << "CPU "    << juce::String (cpuAvg  * 100.0f, 1) << "%  peak "
                   << juce::String (cpuPeak * 100.0f, 1) << "%   ";
    s << "stalled " << juce::String (stalledRatio * 100.0f, 2) << "% (window)   ";
    s << "blocks "  << (juce::int64) processedBlocks
       << " (lifetime stalled "  << (juce::int64) stalledBlocks << ")\n";

    s << "xrun in=" << (juce::int64) engine.getTotalInputOverruns()
      << " out="    << (juce::int64) engine.getTotalOutputUnderruns();

    const double lastUnderrunMs = engine.getMostRecentUnderrunMs();
    if (lastUnderrunMs > 0.0)
    {
        const double agoSec = (juce::Time::getMillisecondCounterHiRes() - lastUnderrunMs) / 1000.0;
        s << "   last dropout " << juce::String (agoSec, 1) << "s ago";
    }
    s << "\n";

    // --- ring fills (first few channels) -------------------------------------
    const int showN = juce::jmin (4, nIn);
    const int showM = juce::jmin (4, nOut);
    s << "inRing[";
    for (int n = 0; n < showN; ++n)
        s << (n ? "," : "") << (juce::int64) engine.getInputRingFill (n);
    s << "]  outRing[";
    for (int m = 0; m < showM; ++m)
        s << (m ? "," : "") << (juce::int64) engine.getOutputRingFill (m);
    s << "]\n\n";

    // --- latency table -------------------------------------------------------
    auto rep = engine.getLatencyReport();
    const double eng = rep.engineSampleRate;
    auto pad = [] (const juce::String& str, int n) -> juce::String
    {
        return str.length() >= n ? str
                                  : (str + juce::String::repeatedString (" ", n - str.length()));
    };
    s << pad ("Device", 28) << pad ("Dir", 6) << pad ("HW spl", 10)
       << pad ("SRC spl", 10) << pad ("Total ms", 10) << "\n";
    for (auto& d : rep.devices)
    {
        if (d.hasInput)
        {
            s << pad (d.name.substring (0, 27), 28)
              << pad ("IN", 6)
              << pad (juce::String (d.hwInputSamples) + "@" + juce::String ((int) d.deviceSampleRate / 1000) + "k", 10)
              << pad (juce::String (d.srcInLatencyEng) + "@" + juce::String ((int) eng / 1000) + "k", 10)
              << pad (juce::String (d.getInputLatencyMs (eng), 2), 10) << "\n";
        }
        if (d.hasOutput)
        {
            s << pad (d.name.substring (0, 27), 28)
              << pad ("OUT", 6)
              << pad (juce::String (d.hwOutputSamples) + "@" + juce::String ((int) d.deviceSampleRate / 1000) + "k", 10)
              << pad (juce::String (d.srcOutLatencyDev) + "@" + juce::String ((int) d.deviceSampleRate / 1000) + "k", 10)
              << pad (juce::String (d.getOutputLatencyMs (eng), 2), 10) << "\n";
        }
    }
    // Break the round-trip down so the math "2.67 + 24.00 + 20.04 = 46.71"
    // is visible -- people see two devices at small ms and are surprised by
    // the larger total without realising the engine itself adds blocks.
    double inMaxMs = 0.0, outMaxMs = 0.0;
    for (auto& d : rep.devices)
    {
        inMaxMs  = juce::jmax (inMaxMs,  d.getInputLatencyMs  (eng));
        outMaxMs = juce::jmax (outMaxMs, d.getOutputLatencyMs (eng));
    }
    const double engMs = rep.getEngineContributionMs();
    s << "\nEngine path  = " << juce::String (engMs, 2) << " ms  ("
      << "1 wait block + " << rep.outputPreFillBlocks << " pre-fill @ "
      << (int) rep.engineBlockSize << " spl / " << (int) eng << " Hz)\n";
    s << "Round-trip worst = " << juce::String (inMaxMs, 2) << " (worst IN)"
      << " + " << juce::String (engMs, 2)    << " (engine)"
      << " + " << juce::String (outMaxMs, 2) << " (worst OUT)"
      << " = " << juce::String (rep.getRoundTripMsWorst(), 2) << " ms";

    if (s != lastBodyText)
    {
        body.setText (s, juce::dontSendNotification);
        lastBodyText = s;
    }

    // --- colour by worst severity --------------------------------------------
    const auto& es = engine.getSettings();
    juce::Colour col = juce::Colour (0xFF000000u | es.accentColorRGB);   // healthy
    const bool recentDropout = lastUnderrunMs > 0.0
        && (juce::Time::getMillisecondCounterHiRes() - lastUnderrunMs) < 5000.0;

    if (cpuAvg >= es.cpuCritRatio || stalledRatio >= es.stalledCritRatio || recentDropout)
        col = juce::Colour (0xFF000000u | es.criticalColorRGB);
    else if (cpuAvg >= es.cpuWarnRatio || stalledRatio >= es.stalledWarnRatio)
        col = juce::Colour (0xFF000000u | es.warningColorRGB);
    body.applyColourToAllText (col, true);

    // Cache values for the gauge strip and trigger its repaint.
    lastCpuAvg        = cpuAvg;
    lastCpuPeak       = cpuPeak;
    lastStalledRatio  = stalledRatio;
    lastXrunIn        = engine.getTotalInputOverruns();
    lastXrunOut       = engine.getTotalOutputUnderruns();
    lastDropoutAgoSec = (lastUnderrunMs > 0.0)
        ? (juce::Time::getMillisecondCounterHiRes() - lastUnderrunMs) / 1000.0
        : -1.0;
    gauges.repaint();
}

void StatusPanel::GaugeStrip::paint (juce::Graphics& g)
{
    const auto& es = owner.engine.getSettings();
    const auto accent   = juce::Colour (0xFF000000u | es.accentColorRGB);
    const auto warning  = juce::Colour (0xFF000000u | es.warningColorRGB);
    const auto critical = juce::Colour (0xFF000000u | es.criticalColorRGB);

    auto pickColour = [&] (float val, float warnAt, float critAt)
    {
        if (val >= critAt)  return critical;
        if (val >= warnAt)  return warning;
        return accent;
    };

    g.fillAll (juce::Colour::fromRGB (16, 16, 20));

    auto bounds = getLocalBounds().reduced (4);
    const int n = 4;
    const int gap = 6;
    const int w = (bounds.getWidth() - (n - 1) * gap) / n;

    // Helper to draw one labeled card with an optional progress bar.
    auto drawCard = [&] (juce::Rectangle<int> r, const juce::String& label,
                          const juce::String& valueText, juce::Colour valueCol,
                          float fillRatio /* 0..1, <0 = no bar */) {
        g.setColour (juce::Colour::fromRGB (24, 24, 30));
        g.fillRect (r);
        g.setColour (juce::Colour::fromRGB (40, 40, 48));
        g.drawRect (r, 1);

        auto inner = r.reduced (6, 4);

        g.setColour (juce::Colour::fromRGB (140, 140, 150));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, 0));
        g.drawText (label, inner.removeFromTop (12), juce::Justification::topLeft);

        g.setColour (valueCol);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 18.0f, juce::Font::bold));
        g.drawText (valueText, inner.removeFromTop (22), juce::Justification::centredLeft);

        if (fillRatio >= 0.0f)
        {
            auto barArea = inner.removeFromTop (8);
            g.setColour (juce::Colour::fromRGB (12, 12, 14));
            g.fillRect (barArea);
            const int filledW = juce::jlimit (0, barArea.getWidth(),
                                              (int) std::round (fillRatio * (float) barArea.getWidth()));
            g.setColour (valueCol);
            g.fillRect (barArea.withWidth (filledW));
            g.setColour (juce::Colour::fromRGB (40, 40, 48));
            g.drawRect (barArea, 0.5f);
        }
    };

    // 1. CPU
    {
        auto r = bounds.withX (bounds.getX()).withWidth (w);
        const auto cpuCol = pickColour (owner.lastCpuAvg, es.cpuWarnRatio, es.cpuCritRatio);
        drawCard (r,
                  "CPU  (peak " + juce::String (owner.lastCpuPeak * 100.0f, 0) + "%)",
                  juce::String (owner.lastCpuAvg * 100.0f, 1) + " %",
                  cpuCol,
                  juce::jlimit (0.0f, 1.0f, owner.lastCpuAvg));
    }
    // 2. Stalled
    {
        auto r = bounds.withX (bounds.getX() + (w + gap)).withWidth (w);
        const auto stCol = pickColour (owner.lastStalledRatio, es.stalledWarnRatio, es.stalledCritRatio);
        drawCard (r,
                  "STALLED (window)",
                  juce::String (owner.lastStalledRatio * 100.0f, 2) + " %",
                  stCol,
                  juce::jlimit (0.0f, 1.0f, owner.lastStalledRatio * 4.0f));   // expand 25% -> full bar
    }
    // 3. Xruns
    {
        auto r = bounds.withX (bounds.getX() + 2 * (w + gap)).withWidth (w);
        const bool dirty = (owner.lastXrunIn + owner.lastXrunOut) > 0;
        drawCard (r, "XRUN  in / out",
                  juce::String ((juce::int64) owner.lastXrunIn) + " / "
                    + juce::String ((juce::int64) owner.lastXrunOut),
                  dirty ? critical : accent,
                  -1.0f);
    }
    // 4. Last dropout
    {
        auto r = bounds.withX (bounds.getX() + 3 * (w + gap)).withWidth (w);
        const bool recent = owner.lastDropoutAgoSec >= 0.0 && owner.lastDropoutAgoSec < 5.0;
        juce::String valText;
        if (owner.lastDropoutAgoSec < 0.0)         valText = "never";
        else if (owner.lastDropoutAgoSec < 60.0)   valText = juce::String (owner.lastDropoutAgoSec, 1) + " s";
        else                                       valText = juce::String ((int) (owner.lastDropoutAgoSec / 60.0)) + "m";
        drawCard (r, "LAST DROPOUT",
                  valText,
                  recent ? critical : accent,
                  -1.0f);
    }
}

} // namespace dcr
