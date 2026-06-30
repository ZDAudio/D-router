#include "UI/PluginEditorWindow.h"

#if JUCE_MAC
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
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

// Hosts a fixed-size plugin editor and scales it so the window can be freely
// resized with the plugin's native aspect ratio kept.
//
// Scaling an arbitrary hosted AU/VST view from the host is genuinely awkward.
// Two approaches that DON'T work here: a juce::AffineTransform on the editor
// (JUCE resizes the plugin's NSView frame, the plugin still draws at its native
// size -> blank/white margins), and a CALayer transform (mis-positions / clips).
//
// What we do instead is the classic AppKit content-zoom: let the editor (and so
// the plugin's NSView frame) fill the holder, then set the plugin view's
// `bounds` back to the natural size.  A view whose bounds are smaller than its
// frame draws its content scaled up to fill the frame -- without us touching the
// plugin's own layout.  (Layer/Metal-backed views may ignore this; then they
// simply stay at natural size in the corner -- best effort, verified per plugin.)
//
// We do NOT own the editor (PluginEditorWindow's unique_ptr does); destroying
// the editor first is safe because Component's dtor removes it from us.
class ScaledEditorHolder : public juce::Component
{
   public:
    ScaledEditorHolder(juce::AudioProcessorEditor& e, int nW, int nH)
        : editor(e), natW(juce::jmax(1, nW)), natH(juce::jmax(1, nH))
    {
        setInterceptsMouseClicks(false, true);
        addAndMakeVisible(editor);
        setSize(natW, natH);
    }

    void resized() override
    {
        // Editor fills the holder, so JUCE sizes the plugin's NSView frame to the
        // scaled size...
        editor.setBounds(0, 0, getWidth(), getHeight());
        // ...then we shrink the plugin view's bounds back to natural so its
        // content scales up to fill that frame.
        applyHostedContentScale();
    }

   private:
#if JUCE_MAC
    // Find the plugin's heavyweight NSView (the largest direct subview of the
    // window's content view -- our holder/editor are lightweight, so the only
    // real subview is the plugin's) and set its bounds to the natural size.
    void applyHostedContentScale()
    {
        auto* peer = getPeer();
        if (peer == nullptr)
            return;
        NSView* content = (NSView*)peer->getNativeHandle();
        if (content == nil)
            return;

        NSView* best = nil;
        CGFloat bestArea = 0.0;
        for (NSView* sub in [content subviews])
        {
            const CGFloat area = sub.frame.size.width * sub.frame.size.height;
            if (area > bestArea)
            {
                bestArea = area;
                best = sub;
            }
        }
        if (best == nil)
            return;

        // bounds (natural) < frame (scaled) => content draws scaled to fill.
        [best setBoundsSize:NSMakeSize((CGFloat)natW, (CGFloat)natH)];
        [best setNeedsDisplay:YES];
    }
#else
    void applyHostedContentScale() {}
#endif

    juce::AudioProcessorEditor& editor;
    int natW, natH;
};
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
        // and express resize policy through the window's existing default
        // constrainer (setResizeLimits / getConstrainer()) -- those do not
        // touch the desktop.  setConstrainer() WOULD (it re-runs
        // setResizable), so we never swap the constrainer pointer.
        const int natW = juce::jmax(1, ed->getWidth());
        const int natH = juce::jmax(1, ed->getHeight());

        // Built-in editors are native JUCE components that relayout in their own
        // resized(), so they must resize FREELY -- never aspect-locked/scaled
        // like a fixed-size external AU NSView.  ("Internal" == our
        // InternalPluginFormat; see BuiltinProcessor::fillInPluginDescription.)
        const bool builtin = (plugin.getPluginDescription().pluginFormatName == "Internal");

        if (ed->isResizable() || builtin)
        {
            // Self-resizing editor -- host it directly, no aspect lock.
            setContentNonOwned(ed, true); // resizes window to editor's natural size
            if (auto* c = (ed->isResizable() ? ed->getConstrainer() : nullptr))
            {
                const int minW = juce::jmax(1, c->getMinimumWidth());
                const int minH = juce::jmax(1, c->getMinimumHeight());
                const int maxW = juce::jmax(minW, c->getMaximumWidth());
                const int maxH = juce::jmax(minH, c->getMaximumHeight());
                setResizeLimits(minW, minH, maxW, maxH);
            }
            else
            {
                // No explicit constrainer: generous free resize around the
                // natural size (half .. 4x), aspect unconstrained.
                const int minW = juce::jmax(1, juce::roundToInt(natW * 0.5));
                const int minH = juce::jmax(1, juce::roundToInt(natH * 0.5));
                setResizeLimits(minW, minH, natW * 4, natH * 4);
            }
        }
        else
        {
            // Fixed-size plugin: wrap it in a scaling holder so the user can
            // resize the window with the native aspect ratio locked.  The
            // plugin can't relayout, so we scale its view via AffineTransform.
            editorHolder = std::make_unique<ScaledEditorHolder>(*ed, natW, natH);
            setContentNonOwned(editorHolder.get(), true);

            const int minW = juce::jmax(1, juce::roundToInt(natW * 0.4));
            const int minH = juce::jmax(1, juce::roundToInt(natH * 0.4));
            setResizeLimits(minW, minH, natW * 3, natH * 3);
            if (auto* c = getConstrainer())
                c->setFixedAspectRatio((double)natW / (double)natH);
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

#if JUCE_MAC
    // Some hosts/plugins leave the window with a full-size content view, which
    // lets the plugin's top row slide under the native title bar.  Clear that
    // style so JUCE's content sits fully below the title bar.
    if (auto* peer = getPeer())
        if (auto* nsv = (NSView*)peer->getNativeHandle())
            if (NSWindow* win = [nsv window])
            {
                win.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
                win.titlebarAppearsTransparent = NO;
            }
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
        clearContentComponent(); // detach the (live) editor/holder from the window
        editorHolder.reset();    // drop the holder (editor is still alive here)
        editor.reset();          // then destroy the editor itself
    }
    @catch (NSException* ex)
    {
        DBG("[plugin editor] NSException during teardown ("
            << plugin.getName() << "): " << [[ex reason] UTF8String]);
        editor.release(); // intentional leak -- safer than crashing
    }
#else
    clearContentComponent();
    editorHolder.reset();
    editor.reset();
#endif
}

void PluginEditorWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

} // namespace dcr
