#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <vector>

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
    //
    // `linkedSiblings`, if non-empty, contains other plugin instances of
    // the SAME plugin type loaded on other (multi-selected) channels'
    // same-index slot.  Every parameter change the user makes in this
    // editor will be mirrored into the sibling instances by parameter
    // index, so a single edit ripples to all selected channels.
    PluginEditorWindow (juce::AudioPluginInstance& p,
                        std::function<void()> onCloseCallback,
                        const juce::String& contextLabel = {},
                        std::vector<juce::AudioPluginInstance*> linkedSiblings = {});
    ~PluginEditorWindow() override;

    void closeButtonPressed() override;

private:
    // Listens for parameter changes on `plugin` and mirrors each change
    // into the same-index parameter on every entry in `siblings`.  The
    // guard flag prevents the recursive fanout that would otherwise hang
    // when JUCE's parameter system notifies us back.
    class ParameterLink : public juce::AudioProcessorListener
    {
    public:
        std::vector<juce::AudioPluginInstance*> siblings;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    private:
        std::atomic<bool> reentry { false };
    };

    juce::AudioPluginInstance&                  plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::function<void()>                       onClose;
    std::unique_ptr<ParameterLink>              paramLink;
};

} // namespace dcr
