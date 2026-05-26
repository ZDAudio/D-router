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

    // Top gauges occupy an 80 px strip -- enough for title + big value +
    // optional bar + "healthy:" hint line per card.  Latency text fills
    // the rest below.
    gauges.setBounds (r.removeFromTop (80));
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
    s << "-- ENGINE ----------------------------------------------\n";
    s << "  Matrix:        " << nIn << " in  ->  " << nOut << " out\n";

    // --- performance line ----------------------------------------------------
    const auto processedBlocks = engine.getMatrixBlocksProcessed();
    const auto stalledBlocks   = engine.getMatrixBlocksStalled();

    // Windowed (per-refresh) metric.  We compute polls-per-block (matrix
    // thread wake-ups per real processed block).  The OLD "stalled %"
    // metric was misleading: at the default 250us poll interval, a healthy
    // engine sits at ~90% stalled because that's just (poll_period /
    // block_period) -- not an audio problem.  polls/block exposes the same
    // underlying counter in a non-alarming form: healthy ~10, problematic
    // when >> 25.
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
        windowPollsPerBlock = dP > 0 ? (float) (dP + dS) / (float) dP : 0.0f;
        prevProcessedBlocks = processedBlocks;
        prevStalledBlocks   = stalledBlocks;
    }

    const float  cpuAvg       = engine.getCpuLoadAvg();
    const float  cpuPeak      = engine.getCpuLoadPeak();
    // Leading indicator of actual underrun risk -- if this drops near 0,
    // the next device callback will xrun.  Two views: percentage (looks
    // bigger when ring is small) and absolute ms (the real "time-to-xrun"
    // figure, independent of ring capacity).
    const float  outRingMin   = engine.getMinOutputRingFillFraction();
    const double outRingMinMs = engine.getMinOutputRingHeadroomMs();

    // Performance section -- formatted as a key/value list with one
    // metric per line so the eye can scan straight down the values.
    s << "\n";
    s << "-- PERFORMANCE -----------------------------------------\n";
    s << "  CPU            " << juce::String (cpuAvg  * 100.0f, 1) << " %"
      << "   (peak " << juce::String (cpuPeak * 100.0f, 1) << " %)\n";
    s << "  Output ring    " << juce::String (outRingMinMs, 1) << " ms safety"
      << "   (" << juce::String (outRingMin * 100.0f, 0) << "% of capacity)\n";
    s << "  Polls / block  " << juce::String (windowPollsPerBlock, 1)
      << "                (efficiency only, not audio)\n";
    s << "  Blocks done    " << (juce::int64) processedBlocks << "\n";

    const auto xIn  = engine.getTotalInputOverruns();
    const auto xOut = engine.getTotalOutputUnderruns();
    s << "  XRUN           "
      << (juce::int64) xIn << " in  /  " << (juce::int64) xOut << " out"
      << ((xIn + xOut) == 0 ? "      (healthy, MUST stay 0)" : "      <-- AUDIBLE GLITCH");

    const double lastUnderrunMs = engine.getMostRecentUnderrunMs();
    if (lastUnderrunMs > 0.0)
    {
        const double agoSec = (juce::Time::getMillisecondCounterHiRes() - lastUnderrunMs) / 1000.0;
        s << "\n  Last dropout   " << juce::String (agoSec, 1) << " s ago";
    }
    s << "\n";

    // --- ring fills (first few channels) -------------------------------------
    const int showN = juce::jmin (4, nIn);
    const int showM = juce::jmin (4, nOut);
    s << "  in ring fill   [";
    for (int n = 0; n < showN; ++n)
        s << (n ? ", " : "") << (juce::int64) engine.getInputRingFill (n);
    if (nIn > showN) s << ", ...";
    s << " ] spl\n";
    s << "  out ring fill  [";
    for (int m = 0; m < showM; ++m)
        s << (m ? ", " : "") << (juce::int64) engine.getOutputRingFill (m);
    if (nOut > showM) s << ", ...";
    s << " ] spl\n\n";

    // --- latency table -------------------------------------------------------
    auto rep = engine.getLatencyReport();
    const double eng = rep.engineSampleRate;
    auto pad = [] (const juce::String& str, int n) -> juce::String
    {
        return str.length() >= n ? str
                                  : (str + juce::String::repeatedString (" ", n - str.length()));
    };
    s << "-- LATENCY ---------------------------------------------\n";
    s << "  " << pad ("Device", 28) << pad ("Dir", 6) << pad ("HW spl", 10)
       << pad ("SRC spl", 10) << pad ("Total ms", 10) << "\n";
    for (auto& d : rep.devices)
    {
        if (d.hasInput)
        {
            s << "  " << pad (d.name.substring (0, 27), 28)
              << pad ("IN", 6)
              << pad (juce::String (d.hwInputSamples) + "@" + juce::String ((int) d.deviceSampleRate / 1000) + "k", 10)
              << pad (juce::String (d.srcInLatencyEng) + "@" + juce::String ((int) eng / 1000) + "k", 10)
              << pad (juce::String (d.getInputLatencyMs (eng), 2), 10) << "\n";
        }
        if (d.hasOutput)
        {
            s << "  " << pad (d.name.substring (0, 27), 28)
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
    s << "\n";
    s << "  Engine path  = " << juce::String (engMs, 2) << " ms"
      << "   (1 wait block + " << rep.outputPreFillBlocks << " pre-fill @ "
      << (int) rep.engineBlockSize << " spl / " << (int) eng << " Hz)\n";
    s << "  Round-trip   = " << juce::String (inMaxMs, 2) << " IN"
      << "  +  " << juce::String (engMs, 2)    << " engine"
      << "  +  " << juce::String (outMaxMs, 2) << " OUT"
      << "  =  " << juce::String (rep.getRoundTripMsWorst(), 2) << " ms";

    if (s != lastBodyText)
    {
        body.setText (s, juce::dontSendNotification);
        lastBodyText = s;
    }

    // --- colour by worst severity --------------------------------------------
    // Severity is driven by what actually matters for audio:
    //   1. CPU above warn/crit
    //   2. Output ring fill below 30% (warn) or 15% (crit) -- LEADING signal
    //   3. A real xrun in the last 5s (definitive)
    // The old code keyed off "stalled%" which was meaningless and turned
    // every healthy session red.
    const auto& es = engine.getSettings();
    juce::Colour col = juce::Colour (0xFF000000u | es.accentColorRGB);   // healthy
    const bool recentDropout = lastUnderrunMs > 0.0
        && (juce::Time::getMillisecondCounterHiRes() - lastUnderrunMs) < 5000.0;

    if (cpuAvg >= es.cpuCritRatio || outRingMinMs < 3.0 || recentDropout)
        col = juce::Colour (0xFF000000u | es.criticalColorRGB);
    else if (cpuAvg >= es.cpuWarnRatio || outRingMinMs < 8.0)
        col = juce::Colour (0xFF000000u | es.warningColorRGB);
    body.applyColourToAllText (col, true);

    // Cache values for the gauge strip and trigger its repaint.
    lastCpuAvg        = cpuAvg;
    lastCpuPeak       = cpuPeak;
    lastOutRingMin    = outRingMin;
    lastOutRingMinMs  = outRingMinMs;
    lastPollsPerBlock = windowPollsPerBlock;
    lastXrunIn        = engine.getTotalInputOverruns();
    lastXrunOut       = engine.getTotalOutputUnderruns();
    lastDropoutAgoSec = (lastUnderrunMs > 0.0)
        ? (juce::Time::getMillisecondCounterHiRes() - lastUnderrunMs) / 1000.0
        : -1.0;
    gauges.repaint();
}

void StatusPanel::GaugeStrip::timerCallback()
{
    // Advance the heartbeat phase by a fixed step each tick.  At 6 Hz this
    // gives a slow, steady pulse the user reads as "engine alive".  The
    // strip is small enough (~80 px tall, full window width) and the paint
    // is just text + filled rects, so a 6 Hz repaint is well under 1% CPU
    // even on a low-end machine.
    pulsePhase += 0.55f;
    if (pulsePhase > 6.2832f) pulsePhase -= 6.2832f;
    repaint();
}

void StatusPanel::GaugeStrip::paint (juce::Graphics& g)
{
    const auto& es = owner.engine.getSettings();
    const auto accent   = juce::Colour (0xFF000000u | es.accentColorRGB);
    const auto warning  = juce::Colour (0xFF000000u | es.warningColorRGB);
    const auto critical = juce::Colour (0xFF000000u | es.criticalColorRGB);

    g.fillAll (juce::Colour::fromRGB (16, 16, 20));

    auto bounds = getLocalBounds().reduced (4);
    const int n = 5;
    const int gap = 6;
    const int w = (bounds.getWidth() - (n - 1) * gap) / n;

    // Pulsing alpha 0.35..1.0 driven by sin(pulsePhase).  Stronger pulse
    // when severity is critical so the eye is drawn to it.
    const float pulse = 0.5f + 0.5f * std::sin (pulsePhase);     // 0..1

    // ---- Card renderer ------------------------------------------------------
    // Layout per card:
    //   [title]                  [● heartbeat]
    //
    //          BIG VALUE
    //   ▓▓▓▓░░░░░░░░░░░░░░░         <- optional fill bar
    //   healthy: <hint>
    auto drawCard = [&] (juce::Rectangle<int> r,
                         const juce::String& title,
                         const juce::String& valueText,
                         const juce::String& healthyHint,
                         juce::Colour valueCol,
                         float fillRatio /* 0..1; <0 = no bar */)
    {
        g.setColour (juce::Colour::fromRGB (24, 24, 30));
        g.fillRect (r);
        g.setColour (juce::Colour::fromRGB (40, 40, 48));
        g.drawRect (r, 1);

        // Heartbeat dot top-right -- pulses alpha at 6 Hz.  Colour matches
        // the card's severity colour so green dots ripple when healthy,
        // red dots throb when not.
        {
            const float dotSize = 7.0f;
            const float margin  = 6.0f;
            const float dx = r.getRight()  - margin - dotSize;
            const float dy = r.getY()      + margin;
            const float alpha = 0.35f + 0.65f * pulse;
            g.setColour (valueCol.withAlpha (alpha));
            g.fillEllipse (dx, dy, dotSize, dotSize);
            // soft outer halo when bright -- 1 ellipse, negligible cost
            g.setColour (valueCol.withAlpha (alpha * 0.20f));
            g.fillEllipse (dx - 2.0f, dy - 2.0f, dotSize + 4.0f, dotSize + 4.0f);
        }

        auto inner = r.reduced (6, 4);

        // Title
        g.setColour (juce::Colour::fromRGB (140, 140, 150));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, 0));
        g.drawText (title, inner.removeFromTop (12).withTrimmedRight (16),
                    juce::Justification::topLeft);

        // BIG value -- the eye-catcher
        g.setColour (valueCol);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      18.0f, juce::Font::bold));
        g.drawText (valueText, inner.removeFromTop (22),
                    juce::Justification::centredLeft);

        if (fillRatio >= 0.0f)
        {
            auto barArea = inner.removeFromTop (6);
            g.setColour (juce::Colour::fromRGB (12, 12, 14));
            g.fillRect (barArea);
            const int filledW = juce::jlimit (0, barArea.getWidth(),
                                              (int) std::round (fillRatio * (float) barArea.getWidth()));
            g.setColour (valueCol);
            g.fillRect (barArea.withWidth (filledW));
            g.setColour (juce::Colour::fromRGB (40, 40, 48));
            g.drawRect (barArea, 0.5f);
            inner.removeFromTop (2);
        }

        // Healthy-range hint at the bottom, dim grey
        g.setColour (juce::Colour::fromRGB (100, 100, 110));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.0f, 0));
        g.drawText ("healthy: " + healthyHint, inner.removeFromTop (12),
                    juce::Justification::topLeft);
    };

    auto pickColour = [&] (float val, float warnAt, float critAt)
    {
        if (val >= critAt)  return critical;
        if (val >= warnAt)  return warning;
        return accent;
    };

    int x = bounds.getX();

    // 1. CPU
    {
        auto r = juce::Rectangle<int> (x, bounds.getY(), w, bounds.getHeight());
        const auto cpuCol = pickColour (owner.lastCpuAvg, es.cpuWarnRatio, es.cpuCritRatio);
        drawCard (r,
                  "CPU  peak " + juce::String (owner.lastCpuPeak * 100.0f, 0) + "%",
                  juce::String (owner.lastCpuAvg * 100.0f, 1) + " %",
                  juce::CharPointer_UTF8 ("\xe2\x86\x93 lower better, < 70%"),
                  cpuCol,
                  juce::jlimit (0.0f, 1.0f, owner.lastCpuAvg));
        x += w + gap;
    }
    // 2. Output ring headroom -- the LEADING indicator of xrun risk.
    //    Show MILLISECONDS as the main value (independent of ring size)
    //    and the % as secondary.  ms is what actually tells you "how
    //    long until xrun if matrix stops producing right now".  warn /
    //    crit thresholds on ms because that's the absolute truth: under
    //    5 ms is risky regardless of how big your ring is.
    {
        auto r = juce::Rectangle<int> (x, bounds.getY(), w, bounds.getHeight());
        juce::Colour ringCol = accent;
        if (owner.lastOutRingMinMs < 3.0)      ringCol = critical;
        else if (owner.lastOutRingMinMs < 8.0) ringCol = warning;
        // Bar normalized: fully filled at 30 ms headroom.
        const float barFill = (float) juce::jlimit (0.0, 1.0, owner.lastOutRingMinMs / 30.0);
        drawCard (r,
                  "OUT RING MIN  xrun risk",
                  juce::String (owner.lastOutRingMinMs, 1) + " ms"
                    + "   (" + juce::String (owner.lastOutRingMin * 100.0f, 0) + "%)",
                  juce::CharPointer_UTF8 ("\xe2\x86\x91 higher better, > 8 ms"),
                  ringCol,
                  barFill);
        x += w + gap;
    }
    // 3. POLLS / BLOCK -- matrix thread wakes per real processed block.
    //    Healthy is roughly (block_period / poll_interval) + 1.
    //    Anything well above that means matrix is hammering rings while
    //    the device callback lags behind.
    {
        auto r = juce::Rectangle<int> (x, bounds.getY(), w, bounds.getHeight());
        juce::Colour pollCol = accent;
        if (owner.lastPollsPerBlock > 50.0f) pollCol = critical;
        else if (owner.lastPollsPerBlock > 25.0f) pollCol = warning;
        drawCard (r,
                  "POLLS / BLOCK  (efficiency)",
                  juce::String (owner.lastPollsPerBlock, 1),
                  "5-15   not an audio metric",
                  pollCol,
                  juce::jlimit (0.0f, 1.0f, owner.lastPollsPerBlock / 40.0f));
        x += w + gap;
    }
    // 4. Xruns -- the only hard truth indicator.  Any > 0 means we shipped
    //    audible glitches; the headline is the count.
    {
        auto r = juce::Rectangle<int> (x, bounds.getY(), w, bounds.getHeight());
        const bool dirty = (owner.lastXrunIn + owner.lastXrunOut) > 0;
        drawCard (r,
                  "XRUN  in / out  (audible)",
                  juce::String ((juce::int64) owner.lastXrunIn) + " / "
                    + juce::String ((juce::int64) owner.lastXrunOut),
                  "MUST be 0",
                  dirty ? critical : accent,
                  -1.0f);
        x += w + gap;
    }
    // 5. Last dropout
    {
        auto r = juce::Rectangle<int> (x, bounds.getY(), w, bounds.getHeight());
        const bool recent = owner.lastDropoutAgoSec >= 0.0 && owner.lastDropoutAgoSec < 5.0;
        juce::String valText;
        if (owner.lastDropoutAgoSec < 0.0)         valText = "never";
        else if (owner.lastDropoutAgoSec < 60.0)   valText = juce::String (owner.lastDropoutAgoSec, 1) + " s";
        else                                       valText = juce::String ((int) (owner.lastDropoutAgoSec / 60.0)) + "m";
        drawCard (r,
                  "LAST DROPOUT",
                  valText,
                  "never",
                  recent ? critical : accent,
                  -1.0f);
    }
}

} // namespace dcr
