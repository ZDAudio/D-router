#include "PanelHost.h"

namespace dcr
{

    namespace
    {
        // Floating window for a detached panel.  Closing it re-embeds the panel
        // via the close callback (which the host wires to PanelHost::reattach).
        class PanelFloatingWindow : public juce::DocumentWindow
        {
        public:
            PanelFloatingWindow (const juce::String& title, std::function<void()> onClose)
                : DocumentWindow (title, juce::Colour::fromRGB (28, 28, 32), DocumentWindow::closeButton),
                  closeFn (std::move (onClose))
            {
                setUsingNativeTitleBar (true);
                setResizable (true, false);
            }

            void closeButtonPressed() override
            {
                if (closeFn)
                    closeFn();
            }

        private:
            std::function<void()> closeFn;
        };
    }

    PanelHost::PanelHost (juce::Component& hostComponent, juce::Component& panelComponent, juce::String windowTitle)
        : host (hostComponent), panel (panelComponent), title (std::move (windowTitle))
    {
    }

    PanelHost::~PanelHost()
    {
        // Drop the non-owned content link before the window dies so it never
        // dereferences a panel that is about to be destroyed.
        if (window)
            window->clearContentComponent();
    }

    void PanelHost::toggle()
    {
        if (state_ == State::Attached)
            detach();
        else
            reattach();
    }

    void PanelHost::detach()
    {
        if (state_ == State::Detached)
            return;

        host.removeChildComponent (&panel);
        window.reset (new PanelFloatingWindow (title, [this] { reattach(); }));
        window->setContentNonOwned (&panel, false);
        const auto sz = windowSize ? windowSize() : juce::Point<int> { 600, 280 };
        window->centreWithSize (sz.x, sz.y);
        window->setVisible (true);
        panel.setVisible (true);

        state_ = State::Detached;
        if (setPanelDetached)
            setPanelDetached (true);
        if (onChanged)
            onChanged();
    }

    void PanelHost::reattach()
    {
        if (state_ == State::Attached)
            return;

        if (window)
        {
            window->clearContentComponent();
            window.reset();
        }
        host.addAndMakeVisible (panel);

        state_ = State::Attached;
        if (setPanelDetached)
            setPanelDetached (false);
        if (onChanged)
            onChanged();
    }

}
