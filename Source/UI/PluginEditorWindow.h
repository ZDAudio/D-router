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
    juce::AudioPluginInstance&                  plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::function<void()>                       onClose;
};

} // namespace dcr
