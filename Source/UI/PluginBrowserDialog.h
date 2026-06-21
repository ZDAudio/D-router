#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace dcr
{

    class AudioEngine;

    // Modal-style picker for AU plugins. Lists everything in the engine's
    // KnownPluginList; the first time it's shown (or when the user hits Rescan)
    // it kicks off a background scan and updates the list as plugins are found.
    class PluginBrowserDialog : public juce::Component, private juce::Timer
    {
    public:
        explicit PluginBrowserDialog (AudioEngine& engine);
        ~PluginBrowserDialog() override;

        // Called with chosen description, or nullopt if cancelled.
        std::function<void (std::optional<juce::PluginDescription>)> onClose;

        void paint (juce::Graphics&) override;
        void resized() override;

        static void launch (AudioEngine& engine,
            std::function<void (std::optional<juce::PluginDescription>)> cb);

    private:
        void timerCallback() override;
        void startScan();
        void stopScan();
        void rebuildList();
        void chooseAndClose (int row);

        AudioEngine& engine;
        juce::ListBox listBox;
        juce::Label title { {}, "Audio Unit plugins" };
        juce::Label status;
        juce::TextButton rescanButton { "Rescan" };
        juce::TextButton cancelButton { "Cancel" };
        juce::TextButton loadButton { "Load" };

        struct ListModel : public juce::ListBoxModel
        {
            explicit ListModel (PluginBrowserDialog& d) : dialog (d) {}
            int getNumRows() override;
            void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override;
            void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
            PluginBrowserDialog& dialog;
        };
        ListModel listModel { *this };

        std::vector<juce::PluginDescription> filteredList;

        // Background scan state
        std::unique_ptr<juce::PluginDirectoryScanner> scanner;
        std::unique_ptr<std::thread> scanThread;
        std::atomic<bool> scanRunning { false };
        std::atomic<bool> stopRequested { false };
        juce::String currentlyScanningFile;
    };

} // namespace dcr
