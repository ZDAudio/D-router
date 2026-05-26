#include "UI/PluginEditorWindow.h"

#if JUCE_MAC
 #import <Foundation/Foundation.h>
#endif

namespace dcr {

namespace
{
    // Defensive editor instantiation: some AU plugins (notably analyzer /
    // metering plugins that lean on OpenGL or assume specific host setup)
    // throw NSException or std::exception from createEditor when run in
    // unfamiliar hosts.  We can't catch a hard SIGSEGV, but exceptions we
    // CAN catch -- the @try/catch wrappers turn an exception-on-editor-open
    // into a graceful fallback to JUCE's generic parameter UI, so a buggy
    // plugin editor never takes down the entire show.
    juce::AudioProcessorEditor* createEditorDefensively (juce::AudioPluginInstance& p)
    {
       #if JUCE_MAC
        juce::AudioProcessorEditor* e = nullptr;
        @try {
            try {
                e = p.createEditorIfNeeded();
            } catch (const std::exception& ex) {
                DBG ("Plugin editor std::exception (" << p.getName() << "): " << ex.what());
                e = nullptr;
            } catch (...) {
                DBG ("Plugin editor unknown C++ exception (" << p.getName() << ")");
                e = nullptr;
            }
        }
        @catch (NSException* ex) {
            DBG ("Plugin editor NSException (" << p.getName() << "): "
                 << [[ex reason] UTF8String]);
            e = nullptr;
        }
        return e;
       #else
        try { return p.createEditorIfNeeded(); }
        catch (...) { return nullptr; }
       #endif
    }
}


void PluginEditorWindow::ScalableHolder::setEditor (juce::AudioProcessorEditor* e)
{
    editor   = e;
    naturalW = e->getWidth();
    naturalH = e->getHeight();
    addAndMakeVisible (editor);
    setSize (naturalW, naturalH);
}

void PluginEditorWindow::ScalableHolder::resized()
{
    if (editor == nullptr) return;

    if (editor->isResizable())
    {
        // Plugin handles its own resize; let it fill us at 1:1.
        editor->setTransform (juce::AffineTransform());
        editor->setBounds (getLocalBounds());
        return;
    }

    // Fixed-size editor: scale-to-fit, preserving aspect, with letterboxing.
    if (naturalW <= 0 || naturalH <= 0) { editor->setBounds (getLocalBounds()); return; }
    const float s = juce::jmin ((float) getWidth()  / (float) naturalW,
                                (float) getHeight() / (float) naturalH);
    const int rW = (int) std::round (naturalW * s);
    const int rH = (int) std::round (naturalH * s);
    const int x  = (getWidth()  - rW) / 2;
    const int y  = (getHeight() - rH) / 2;
    editor->setBounds (0, 0, naturalW, naturalH);
    editor->setTransform (juce::AffineTransform::scale (s).translated ((float) x, (float) y));
}

PluginEditorWindow::PluginEditorWindow (juce::AudioPluginInstance& p,
                                        std::function<void()> cb,
                                        const juce::String& contextLabel)
    : DocumentWindow (contextLabel.isNotEmpty()
                          ? (contextLabel + "  -  " + p.getName())
                          : p.getName(),
                      juce::Colour::fromRGB (40, 40, 46),
                      DocumentWindow::closeButton),
      plugin (p),
      onClose (std::move (cb))
{
    setUsingNativeTitleBar (true);

    // CRITICAL ORDER (Youlean Loudness Meter 2 crash, May 2026):
    //
    // The previous order was:
    //   1. holder.setEditor(plugin editor)         <- plugin NSView -> holder
    //   2. setContentNonOwned(holder, true)        <- holder -> window content
    //   3. setResizable(true, false)               <- forces addToDesktop()
    //
    // Step 3 sent -[NSView _setWindow:] DOWN the existing NSView tree, which
    // includes the plugin's NSView -- but the host NSWindow's native peer
    // wasn't fully realised yet (backingScaleFactor returns nil, etc.).
    // Youlean's NSView's _setWindow: handler dereferences a nil window
    // property and SIGSEGVs.  Stack frames 9->8->5->3 in the crash log.
    //
    // Reordered:
    //   a. realise the empty window (setResizable triggers addToDesktop
    //      while there's no plugin NSView attached) and place it on screen
    //   b. THEN install the editor content -- the plugin's NSView migrates
    //      from "no parent" to a fully-alive NSWindow, which is a path
    //      plugins handle robustly because it's how every regular DAW
    //      opens a plugin window.
    setResizable (true, false);
    centreWithSize (400, 300);   // placeholder; resized once editor lands
    setVisible    (true);
    toFront       (true);

    // Plugin's native editor first, fall back to generic param view if it
    // returns null OR throws.
    juce::AudioProcessorEditor* e = createEditorDefensively (plugin);
    if (e == nullptr)
    {
        DBG ("Falling back to GenericAudioProcessorEditor for " << plugin.getName());
        e = new juce::GenericAudioProcessorEditor (plugin);
    }
    editor.reset (e);

    // Attach + size now that the window has a real peer.  Still @try-wrapped
    // -- some plugins still throw inside their layout pass on first attach.
   #if JUCE_MAC
    @try {
        holder.setEditor (e);
        setContentNonOwned (&holder, true);
        centreWithSize (holder.getWidth(), holder.getHeight());
    }
    @catch (NSException* ex) {
        DBG ("Plugin editor NSException during attach (" << plugin.getName()
             << "): " << [[ex reason] UTF8String]);
        editor.reset (new juce::GenericAudioProcessorEditor (plugin));
        holder.setEditor (editor.get());
        setContentNonOwned (&holder, true);
        centreWithSize (holder.getWidth(), holder.getHeight());
    }
   #else
    holder.setEditor (e);
    setContentNonOwned (&holder, true);
    centreWithSize (holder.getWidth(), holder.getHeight());
   #endif
}

PluginEditorWindow::~PluginEditorWindow()
{
    // Detach the editor before deleting the unique_ptr - the plugin still
    // owns the editor's processor reference.
    clearContentComponent();
    editor.reset();
}

void PluginEditorWindow::closeButtonPressed()
{
    if (onClose) onClose();
}

} // namespace dcr
