#include "UI/StatusPanel.h"

#include "Engine/AudioEngine.h"

namespace dcr {

StatusPanel::StatusPanel (AudioEngine& e) : engine (e)
{
    title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible (title);

    popOutBtn.setTooltip ("Pop status panel out into its own window");
    popOutBtn.onClick = [this] { if (onPopOutRequested) onPopOutRequested(); };
    addAndMakeVisible (popOutBtn);

    body.setMultiLine (true);
    body.setReadOnly (true);
    body.setScrollbarsShown (true);
    body.setCaretVisible (false);
    body.setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (18, 18, 22));
    body.setColour (juce::TextEditor::textColourId,       juce::Colour::fromRGB (200, 220, 200));
    body.setFont   (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, 0));
    addAndMakeVisible (body);

    startTimer (engine.getSettings().statusTimerMs);
    refresh();
}

void StatusPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (18, 18, 22));
}

void StatusPanel::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto top = r.removeFromTop (20);
    popOutBtn.setBounds (top.removeFromRight (32));
    title.setBounds (top);
    r.removeFromTop (4);
    body.setBounds (r);
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
    s << "\nEngine path  = " << juce::String (rep.getEngineContributionMs(), 2) << " ms  ("
      << "1 wait block + " << rep.outputPreFillBlocks << " pre-fill @ "
      << (int) rep.engineBlockSize << " spl / " << (int) eng << " Hz)\n";
    s << "Round-trip worst = " << juce::String (rep.getRoundTripMsWorst(), 2) << " ms";

    body.setText (s, juce::dontSendNotification);

    // --- colour by worst severity --------------------------------------------
    const auto& es = engine.getSettings();
    juce::Colour col = juce::Colour::fromRGB (200, 220, 200);   // healthy
    const bool recentDropout = lastUnderrunMs > 0.0
        && (juce::Time::getMillisecondCounterHiRes() - lastUnderrunMs) < 5000.0;

    if (cpuAvg >= es.cpuCritRatio || stalledRatio >= es.stalledCritRatio || recentDropout)
        col = juce::Colour::fromRGB (240, 80, 70);
    else if (cpuAvg >= es.cpuWarnRatio || stalledRatio >= es.stalledWarnRatio)
        col = juce::Colour::fromRGB (240, 200, 60);
    body.applyColourToAllText (col, true);
}

} // namespace dcr
