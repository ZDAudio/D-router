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

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override { refresh(); }
    void refresh();

    AudioEngine&     engine;
    bool             detached = false;

    juce::Label      title    { {}, "Engine status" };
    juce::TextButton popOutBtn { "->" };
    juce::TextEditor body;

    // Window-based rate tracking so the displayed ratios reflect the LAST
    // refresh interval, not lifetime cumulative.  Otherwise the startup
    // stall (matrix polled before audio arrived) pins ratio at 100% forever.
    bool     firstSample        = true;
    uint64_t prevProcessedBlocks = 0;
    uint64_t prevStalledBlocks   = 0;
    float    windowStalledRatio  = 0.0f;
};

} // namespace dcr
