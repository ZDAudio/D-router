#include "UI/PluginEditorWindow.h"

#if JUCE_MAC
#import <Foundation/Foundation.h>
#endif

namespace dcr
{

namespace
{
// Defensive editor instantiation: some AU plugins (notably analyzer /
// metering plugins that lean on OpenGL or assume specific host setup)
// throw NSException or std::exception from createEditor when run in
// unfamiliar hosts.  We can't catch a hard SIGSEGV, but exceptions we
// CAN catch -- the @try/catch wrappers turn an exception-on-editor-open
// into a graceful fallback to JUCE's generic parameter UI, so a buggy
// plugin editor never takes down the entire show.
juce::AudioProcessorEditor* createEditorDefensively(juce::AudioPluginInstance& p)
{
#if JUCE_MAC
    juce::AudioProcessorEditor* e = nullptr;
    @try
    {
        try
        {
            e = p.createEditorIfNeeded();
        }
        catch (const std::exception& ex)
        {
            DBG("Plugin editor std::exception (" << p.getName() << "): " << ex.what());
            e = nullptr;
        }
        catch (...)
        {
            DBG("Plugin editor unknown C++ exception (" << p.getName() << ")");
            e = nullptr;
        }
    }
    @catch (NSException* ex)
    {
        DBG("Plugin editor NSException (" << p.getName() << "): "
                                          << [[ex reason] UTF8String]);
        e = nullptr;
    }
    return e;
#else
    try
    {
        return p.createEditorIfNeeded();
    }
    catch (...)
    {
        return nullptr;
    }
#endif
}
} // namespace

void PluginEditorWindow::ParameterLink::audioProcessorParameterChanged(
    juce::AudioProcessor*,
    int parameterIndex,
    float newValue)
{
    // Guard against the recursive notification storm that would follow
    // when each sibling's setValueNotifyingHost fires our own listener
    // back through the same source's listener chain.  Only the FIRST
    // entry on the message thread proceeds; nested entries early-out.
    bool expected = false;
    if (!reentry.compare_exchange_strong(expected, true))
        return;

    for (auto* sib : siblings)
    {
        if (sib == nullptr)
            continue;
        const auto& params = sib->getParameters();
        if (parameterIndex >= 0 && parameterIndex < params.size())
            if (auto* param = params[parameterIndex])
                param->setValueNotifyingHost(newValue);
    }
    reentry.store(false);
}

PluginEditorWindow::PluginEditorWindow(juce::AudioPluginInstance& p,
                                       std::function<void()> cb,
                                       const juce::String& contextLabel,
                                       std::vector<juce::AudioPluginInstance*> linkedSiblings)
    : DocumentWindow((contextLabel.isNotEmpty()
                          ? (contextLabel + "  -  " + p.getName())
                          : p.getName()) +
                         (linkedSiblings.empty()
                              ? juce::String{}
                              : juce::String("   [linked x") + juce::String((int)linkedSiblings.size() + 1) + "]"),
                     juce::Colour::fromRGB(40, 40, 46),
                     DocumentWindow::closeButton),
      plugin(p),
      onClose(std::move(cb))
{
    // Install param-link listener if any siblings were provided.  Siblings
    // must be the SAME plugin type (broadcast load guarantees this since
    // every selected channel was loaded from the same desc), so parameter
    // index N on `plugin` matches parameter index N on every sibling.
    if (!linkedSiblings.empty())
    {
        paramLink = std::make_unique<ParameterLink>();
        paramLink->siblings = std::move(linkedSiblings);
        plugin.addListener(paramLink.get());
    }

    setUsingNativeTitleBar(true);

    // CRITICAL ORDER (a known GPU-backed metering plug-in crash):  realise the empty
    // NSWindow on the desktop BEFORE attaching the plugin's NSView, so the
    // plugin's -[NSView _setWindow:] handler sees a fully-alive host
    // window (not a half-built one with nil backingScaleFactor).
    setResizable(true, false);
    centreWithSize(400, 300);
    setVisible(true);
    toFront(true);

    // Build the editor (defensively -- crashy plugins fall back to the
    // generic parameter UI rather than taking the app down).
    juce::AudioProcessorEditor* e = createEditorDefensively(plugin);
    if (e == nullptr)
    {
        DBG("Falling back to GenericAudioProcessorEditor for " << plugin.getName());
        e = new juce::GenericAudioProcessorEditor(plugin);
    }
    editor.reset(e);

    auto installEditor = [this](juce::AudioProcessorEditor* ed)
    {
        // CRITICAL: do NOT call setResizable a second time here.  JUCE's
        // ResizableWindow::setResizable internally re-invokes addToDesktop
        // (to refresh desktop window style flags) whenever isOnDesktop()
        // is true.  That re-addToDesktop sends -[NSView _setWindow:]
        // through the entire NSView tree -- which by now includes the
        // freshly-attached plugin NSView.  Such a plug-in's
        // _setWindow: handler crashes on this second migration even
        // though the first one (no plugin attached yet) was fine.
        //
        // Trick instead: leave setResizable(true) from the ctor preamble
        // and use setResizeLimits to express "this plugin's editor can /
        // can't be resized".  setResizeLimits doesn't touch the desktop.
        setContentNonOwned(ed, true); // resizes window to editor's natural size

        const int natW = juce::jmax(1, ed->getWidth());
        const int natH = juce::jmax(1, ed->getHeight());

        if (ed->isResizable())
        {
            if (auto* c = ed->getConstrainer())
            {
                const int minW = juce::jmax(1, c->getMinimumWidth());
                const int minH = juce::jmax(1, c->getMinimumHeight());
                const int maxW = juce::jmax(minW, c->getMaximumWidth());
                const int maxH = juce::jmax(minH, c->getMaximumHeight());
                setResizeLimits(minW, minH, maxW, maxH);
            }
            else
            {
                // No explicit constrainer: pin the lower bound at the
                // natural size, give a generous upper bound.
                setResizeLimits(natW, natH, natW * 8, natH * 8);
            }
        }
        else
        {
            // Fixed-size plugin: lock the window to the natural size by
            // setting min == max.  No setResizable(false) call -- it
            // would addToDesktop again.
            setResizeLimits(natW, natH, natW, natH);
        }
        centreWithSize(getWidth(), getHeight());
    };

#if JUCE_MAC
    @try
    {
        installEditor(editor.get());
    }
    @catch (NSException* ex)
    {
        DBG("Plugin editor NSException during attach (" << plugin.getName()
                                                        << "): " << [[ex reason] UTF8String]);
        editor.reset(new juce::GenericAudioProcessorEditor(plugin));
        installEditor(editor.get());
    }
#else
    installEditor(editor.get());
#endif
}

PluginEditorWindow::~PluginEditorWindow()
{
    // Detach the parameter listener BEFORE we tear down anything else --
    // a stray callback after editor.reset() could try to mirror into
    // siblings while we're mid-destruction.
    if (paramLink != nullptr)
        plugin.removeListener(paramLink.get());

    // Hide the window FIRST so a GPU-backed plug-in view isn't being
    // composited while its NSView is detached /
    // destroyed.  Tearing those views down while they're still on screen has
    // crashed the app (SIGSEGV deep in the AU's own view teardown).
    setVisible(false);

#if JUCE_MAC
    // Guard the teardown: some AU editors throw NSException out of their
    // Cocoa view destruction.  If that happens, LEAK the editor (and its
    // view) rather than let the unhandled exception take the whole app down
    // -- a small leak on plugin-window close is vastly better than a crash
    // that kills every audio route.
    @try
    {
        clearContentComponent(); // detach the (live) editor from the window
        editor.reset();          // then destroy it
    }
    @catch (NSException* ex)
    {
        DBG("[plugin editor] NSException during teardown ("
            << plugin.getName() << "): " << [[ex reason] UTF8String]);
        editor.release(); // intentional leak -- safer than crashing
    }
#else
    clearContentComponent();
    editor.reset();
#endif
}

void PluginEditorWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

} // namespace dcr
