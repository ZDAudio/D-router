#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr {

// Semi-transparent full-window overlay that shows a centered title +
// progress message during long-running startup / reconfigure steps so
// the user doesn't think the app froze.  Owned by MainComponent;
// MatrixView fires onRebuildProgress / onRebuildFinished into it.
//
// Also used as the startup splash -- it's visible from the moment
// MainComponent is constructed and stays until the first
// matrixView.onRebuildFinished fires (engine started + UI built).
class LoadingOverlay : public juce::Component
{
public:
    LoadingOverlay();

    void paint (juce::Graphics&) override;
    void resized() override;

    void setMessage (const juce::String& message);
    void setProgress (int doneSteps, int totalSteps);   // <0 totalSteps == indeterminate
    void hideOverlay();
    void showOverlay (const juce::String& message);

private:
    juce::String title    { "ZDAudio  D-Router" };
    juce::String subtitle { "Loading..." };
    int    progressDone   = 0;
    int    progressTotal  = 0;
};

} // namespace dcr
