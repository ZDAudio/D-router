#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace dcr {

// Floating window that hosts a plugin's AudioProcessorEditor (or a generic
// editor fallback if the plugin doesn't provide one). Calls onCloseCallback
// when the user closes the window.
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    // `contextLabel`, if non-empty, is prepended to the window title so
    // popped-out plugin editors visibly identify which track / group / slot
    // they belong to (e.g. "OUTPUT BlackHole 2ch ch.1 / slot 2 — AUCompressor").
    PluginEditorWindow (juce::AudioPluginInstance& p,
                        std::function<void()> onCloseCallback,
                        const juce::String& contextLabel = {});
    ~PluginEditorWindow() override;

    void closeButtonPressed() override;

private:
    // Wraps the plugin's editor; for fixed-size editors, applies an
    // AffineTransform::scale so the window can still be made bigger / smaller
    // while preserving aspect ratio.  Blurry-but-readable is preferred to
    // empty padding.
    class ScalableHolder : public juce::Component
    {
    public:
        void setEditor (juce::AudioProcessorEditor* e);
        void resized() override;
    private:
        juce::AudioProcessorEditor* editor = nullptr;
        int naturalW = 0;
        int naturalH = 0;
    };

    juce::AudioPluginInstance&                  plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    ScalableHolder                              holder;
    std::function<void()>                       onClose;
};

} // namespace dcr
