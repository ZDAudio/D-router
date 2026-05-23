#include <juce_gui_basics/juce_gui_basics.h>

#include "MainComponent.h"

namespace dcr {

class DcoreRouterApp : public juce::JUCEApplication
{
public:
    DcoreRouterApp() = default;

    const juce::String getApplicationName()    override { return "D-Core Router"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed()          override { return false; }

    void initialise (const juce::String&) override
    {
        mainWindow.reset (new MainWindow (getApplicationName()));
    }

    void shutdown() override { mainWindow = nullptr; }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (std::move (name),
                              juce::Colours::darkgrey,
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (760, 620, 4096, 2400);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace dcr

START_JUCE_APPLICATION (dcr::DcoreRouterApp)
