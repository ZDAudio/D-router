#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace dcr {

class AudioEngine;

// Combined diagnostics + latency panel.  Embedded by default at the bottom of
// the main window; can be popped out into its own DocumentWindow like the
// OutputGroupPanel.  All formerly-status-bar info and the latency table live
// here.
class StatusPanel : public juce::Component, private juce::Timer
{
public:
    explicit StatusPanel (AudioEngine& engine);

    // Pop-out button asks the host (MainComponent) to detach/re-attach.
    std::function<void()> onPopOutRequested;
    void setDetached (bool d) { detached = d; popOutBtn.setButtonText (d ? "<-" : "->"); }

    // Force an immediate refresh (e.g. after engine restart).
    void refreshNow() { refresh(); }

    // Cmd+= / Cmd+- / Cmd+0 hooks from MainComponent.  delta > 0 grows
    // by 1 pt, < 0 shrinks, == 0 resets to the 11 pt default.  Clamped
    // to [8, 24] so the user can't accidentally make text unreadable.
    void bumpBodyFontSize (int delta);

    // Reset xrun counters across all device workers.  Used by the
    // "Reset" button next to the XRUN gauge so the user can zero the
    // cold-start noise once the engine has stabilised.
    void resetXrun();

    // Freeze polling while the engine is being reconfigured -- otherwise the
    // timer can iterate workers / read inputPeaks while they're being torn
    // down / moved by AudioEngine::stop()+start() on the worker thread.
    void pauseUpdates()  { stopTimer(); }
    void resumeUpdates();

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void timerCallback() override { refresh(); }
    void refresh();

    AudioEngine&     engine;
    bool             detached = false;

    juce::Label      title    { {}, "Engine status" };
    juce::TextButton popOutBtn { "->" };
    juce::TextButton resetXrunBtn { "Reset XRUN" };

    float            bodyFontPt = 11.0f;   // adjusted via Cmd+= / Cmd+-

    // Self-painted gauge strip at the top of the panel; the body below is
    // still a monospaced TextEditor for the latency table.
    //
    // Runs a low-frequency (6 Hz) timer that drives a subtle heartbeat
    // pulse on each card's status dot.  Only this small strip repaints --
    // not the body text -- and the paint is just text + filled rects, so
    // the animation cost is in the noise.
    class GaugeStrip : public juce::Component,
                       private juce::Timer
    {
    public:
        explicit GaugeStrip (StatusPanel& o) : owner (o) { startTimerHz (6); }
        void paint (juce::Graphics&) override;
    private:
        void timerCallback() override;
        StatusPanel& owner;
        float pulsePhase = 0.0f;   // 0..2π, advanced on each tick
    };
    GaugeStrip       gauges { *this };
    juce::TextEditor body;

    // Cached metrics for the gauge paint() so we don't refetch on every
    // expose; updated in refresh().
    float    lastCpuAvg        = 0.0f;
    float    lastCpuPeak       = 0.0f;
    float    lastOutRingMin    = 1.0f;   // 0..1, leading xrun-risk indicator
    double   lastOutRingMinMs  = 0.0;    // absolute headroom in ms (the actionable metric)
    float    lastPollsPerBlock = 0.0f;   // matrix wakes per real block
    uint64_t lastXrunIn        = 0;
    uint64_t lastXrunOut       = 0;
    double   lastDropoutAgoSec = -1.0;   // -1 = never
    juce::String lastBodyText;            // skip setText when unchanged

    // Window-based rate tracking so the displayed ratios reflect the LAST
    // refresh interval, not lifetime cumulative.
    bool     firstSample          = true;
    uint64_t prevProcessedBlocks  = 0;
    uint64_t prevStalledBlocks    = 0;
    float    windowPollsPerBlock  = 0.0f;
};

} // namespace dcr
