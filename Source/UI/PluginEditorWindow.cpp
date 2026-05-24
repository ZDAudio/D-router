#include "UI/PluginEditorWindow.h"

namespace dcr {

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

    juce::AudioProcessorEditor* e = plugin.createEditorIfNeeded();
    if (e == nullptr)
        e = new juce::GenericAudioProcessorEditor (plugin);
    editor.reset (e);

    holder.setEditor (e);
    setContentNonOwned (&holder, true);   // window adopts holder's initial natural size

    // Always allow user resize.  Fixed-size plugins get smooth-scaled (blurry
    // is OK); resizable plugins forward the new size to the plugin itself.
    setResizable (true, false);

    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    toFront (true);
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
