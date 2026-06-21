#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace dcr
{

    // One detachable UI panel slot (Phase C2).  Before this, MainComponent
    // carried three near-identical copies of "detached bool + DocumentWindow +
    // toggle method" (status / input-group / output-group panels).  This
    // collapses them into a single parameterized owner with an explicit
    // Attached | Detached state.
    //
    // Message-thread only.  Holds a non-owning reference to the host (the embed
    // target) and the panel; owns the floating window while Detached.  The
    // owner reads isDetached() to drive its layout, and wires three hooks:
    //   - windowSize       : floating-window size at detach time (lazy, because
    //                        some panels size off a dynamic content width).
    //   - setPanelDetached : forwards to the panel's typed setDetached(bool).
    //   - onChanged        : invoked after every transition so the owner can
    //                        re-run its tab layout.
    class PanelHost
    {
    public:
        enum class State {
            Attached,
            Detached
        };

        PanelHost (juce::Component& hostComponent, juce::Component& panelComponent, juce::String windowTitle);
        ~PanelHost();

        State state() const noexcept { return state_; }
        bool isDetached() const noexcept { return state_ == State::Detached; }

        std::function<juce::Point<int>()> windowSize;
        std::function<void (bool)> setPanelDetached;
        std::function<void()> onChanged;

        // Flip between embedded and floating.
        void toggle();

        // Force back to Attached if floating (teardown / engine restart).
        void reattach();

    private:
        void detach();

        juce::Component& host;
        juce::Component& panel;
        juce::String title;
        State state_ = State::Attached;
        std::unique_ptr<juce::DocumentWindow> window;
    };

}
