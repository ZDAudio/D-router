#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "Diagnostics/CrashHandler.h"
#include "Diagnostics/Logger.h"
#include "MainComponent.h"

namespace dcr {

namespace
{
    // Render the menu-bar glyph programmatically so we don't ship an asset.
    // A 3x3 routing-matrix grid with the diagonal crosspoints lit -- reads
    // clearly even at the ~22 px macOS menu-bar size.  `tint` lets us make a
    // teal colour version (Windows/Linux tray) and a black template version
    // (macOS, which re-tints by alpha for light/dark menu bars).
    juce::Image makeTrayGlyph (juce::Colour tint)
    {
        constexpr int S = 44;                    // 2x for retina; the OS scales down
        juce::Image img (juce::Image::ARGB, S, S, true);
        juce::Graphics g (img);
        g.setColour (tint);

        constexpr int n = 3;
        const float pad  = 6.0f;
        const float cell = (S - 2.0f * pad) / (float) n;
        const float r    = cell * 0.28f;
        for (int row = 0; row < n; ++row)
            for (int col = 0; col < n; ++col)
            {
                const float cx = pad + cell * (col + 0.5f);
                const float cy = pad + cell * (row + 0.5f);
                if (row == col)                  // lit crosspoints on the diagonal
                    g.fillEllipse (cx - r, cy - r, 2 * r, 2 * r);
                else
                    g.drawEllipse (cx - r, cy - r, 2 * r, 2 * r, 1.2f);
            }
        return img;
    }
}

class DcoreRouterApp : public juce::JUCEApplication
{
public:
    DcoreRouterApp() = default;

    const juce::String getApplicationName()    override { return "ZDAudio D-Router"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed()          override { return false; }

    void initialise (const juce::String&) override
    {
        // Logger first so the crash handler has a file to append to, and
        // so every subsequent DBG / juce::Logger::writeToLog flows through.
        Logger::init();
        CrashHandler::install();
        mainWindow.reset (new MainWindow (getApplicationName()));

        // Menu-bar-only presence: drop the Dock icon entirely and live in the
        // status bar.  The red close button HIDES the window (engine keeps
        // running in the background); the tray menu shows it again or quits
        // for real.  No Dock icon means we never have to juggle a Dock-reopen
        // event -- the tray icon is the single, always-present entry point.
        trayIcon.reset (new TrayIcon());
        trayIcon->setIconImage (makeTrayGlyph (juce::Colour::fromRGB (0, 255, 210)),  // colour (Win/Linux)
                                makeTrayGlyph (juce::Colours::black));                 // template (macOS)
        trayIcon->setIconTooltip ("ZDAudio D-Router");
        trayIcon->isWindowVisible = [this] { return mainWindow != nullptr && mainWindow->isVisible(); };
        trayIcon->onToggleWindow  = [this] { toggleWindow(); };
        trayIcon->onQuit          = [this] { quit(); };

        juce::Process::setDockIconVisible (false);
        mainWindow->toFront (true);
    }

    void shutdown() override
    {
        trayIcon   = nullptr;
        mainWindow = nullptr;
        Logger::shutdown();
    }

    // Cmd+Q / system logout / shutdown -> really quit (save + clean exit).
    // Only the red close button hides to the tray.
    void systemRequestedQuit() override { quit(); }

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

        // Hide to the menu bar instead of quitting.  The audio engine and all
        // timers keep running; the tray menu brings the window back.
        void closeButtonPressed() override { setVisible (false); }
    };

private:
    class TrayIcon : public juce::SystemTrayIconComponent
    {
    public:
        std::function<bool()> isWindowVisible;
        std::function<void()> onToggleWindow;
        std::function<void()> onQuit;

        void mouseDown (const juce::MouseEvent&) override
        {
            const bool vis = isWindowVisible && isWindowVisible();
            juce::PopupMenu m;
            m.addItem (vis ? "Hide D-Router" : "Show D-Router",
                       [this] { if (onToggleWindow) onToggleWindow(); });
            m.addSeparator();
            m.addItem ("Quit D-Router", [this] { if (onQuit) onQuit(); });
            showDropdownMenu (m);
        }
    };

    void toggleWindow()
    {
        if (mainWindow == nullptr) return;
        if (mainWindow->isVisible())
        {
            mainWindow->setVisible (false);
        }
        else
        {
            mainWindow->setVisible (true);
            mainWindow->toFront (true);
            // Accessory apps don't auto-activate; pull us to the foreground so
            // the window actually gets keyboard focus and comes to the front.
            juce::Process::makeForegroundProcess();
        }
    }

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<TrayIcon>   trayIcon;
};

} // namespace dcr

START_JUCE_APPLICATION (dcr::DcoreRouterApp)
