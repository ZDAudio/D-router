#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr
{

    // Shows the current session log + a button to reveal it (or its containing
    // folder) in Finder.  Auto-refreshes every second while open.  Use to
    // inspect what the app was doing right before a crash without leaving the
    // app or grepping ~/Library/Logs by hand.
    class LogViewerDialog : public juce::Component,
                            private juce::Timer
    {
    public:
        LogViewerDialog();
        ~LogViewerDialog() override = default;

        void paint (juce::Graphics&) override;
        void resized() override;

        static void launch();

    private:
        void timerCallback() override;
        void refresh();

        juce::Label header;
        juce::TextEditor body;
        juce::TextButton refreshBtn { "Refresh" };
        juce::TextButton revealBtn { "Reveal in Finder" };
        juce::TextButton copyPathBtn { "Copy log path" };
        juce::TextButton closeBtn { "Close" };
    };

} // namespace dcr
